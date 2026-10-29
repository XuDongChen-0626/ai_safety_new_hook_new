/*
 * offline_log_sd.c - SD card mount/writer for the offline event log.
 *
 * File layout on the FAT32 SD card:
 *
 *   /SD:/olog/pending/hook_pending_boot_000001.log          (no trusted time)
 *   /SD:/olog/2026-09-05/hook_2026-09-05_16-20-33_000001.log
 *
 * The file name prefix (CONFIG_OFFLINE_LOG_FILE_PREFIX, default "hook")
 * identifies the product so logs pulled from different controllers stay
 * distinguishable after they are copied to the PC.
 *
 * Design rules inherited from the craner_encoder_hub_ulog project:
 *  - All SD I/O happens in one low-priority writer thread; business threads
 *    only enqueue into a RAM message queue (drop-oldest on overflow).
 *  - Every record is fsync()ed right away because event volume is low and
 *    the content is safety-relevant.
 *  - When a trusted time source appears the pending file is renamed into the
 *    date directory; after that, daily files are created lazily.
 *  - Date directories older than OFFLINE_LOG_RETENTION_DAYS are removed and
 *    a total-size cap (OFFLINE_LOG_MAX_TOTAL_KB) protects the SD card.
 */

#include "offline_log_internal.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zephyr/app_version.h>

#if defined(CONFIG_DEVICE_IDENTITY_SERVICE)
#include "device_identity_service.h"
#endif
#if defined(CONFIG_TIME_MANAGER_SERVICE)
#include "time_manager_service.h"
#endif
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(offline_log_sd, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * Paths are assembled with checked snprintf() results and directory names
 * are validated before use. GCC's -Wformat-truncation analysis cannot see
 * those runtime guarantees for FATFS entry names, so it is disabled here.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#define OL_QUEUE_SIZE    CONFIG_OFFLINE_LOG_QUEUE_SIZE
#define OL_MSG_MAX       CONFIG_OFFLINE_LOG_MSG_MAX
#define OL_MAX_FILE_BYTES ((uint64_t)CONFIG_OFFLINE_LOG_MAX_FILE_KB * 1024U)
#define OL_MAX_TOTAL_BYTES ((uint64_t)CONFIG_OFFLINE_LOG_MAX_TOTAL_KB * 1024U)
#define OL_PENDING_MAX_TOTAL_BYTES \
	((uint64_t)CONFIG_OFFLINE_LOG_PENDING_MAX_TOTAL_KB * 1024U)
#define OL_PENDING_CLEANUP_MAX_FILES 32U
#define OL_RETENTION_DAYS CONFIG_OFFLINE_LOG_RETENTION_DAYS
#define OL_FREE_MIN_KB CONFIG_OFFLINE_LOG_FREE_SPACE_MIN_KB
#define OL_FREE_MIN_PERCENT CONFIG_OFFLINE_LOG_FREE_SPACE_MIN_PERCENT
#define OL_CLEANUP_MAX_DIRS CONFIG_OFFLINE_LOG_CLEANUP_MAX_DIRS
#define OL_CLEANUP_INTERVAL_S 3600U
#define OL_SPACE_CHECK_INTERVAL_S 300U
#define OL_MOUNT_MAX_ATTEMPTS CONFIG_OFFLINE_LOG_MOUNT_MAX_ATTEMPTS
#define OL_DISK_RESET_SETTLE_MS 100U
#define OL_CARD_SETTLE_MS 1000U

/* File name prefix, e.g. "car_flat" / "car_luffing". */
#define OL_FILE_PREFIX CONFIG_OFFLINE_LOG_FILE_PREFIX
#define OL_FILE_PREFIX_LEN (sizeof(OL_FILE_PREFIX) - 1U)

#if defined(CONFIG_TIME_LOCAL_FORMAT)
#define OL_TZ_OFFSET_MINUTES CONFIG_TIME_LOCAL_TIMEZONE_OFFSET_MINUTES
#else
#define OL_TZ_OFFSET_MINUTES 0
#endif

/* One queued record (timestamp is added by the writer). */
struct offline_log_record {
	uint8_t level;
	uint8_t category;
	char text[OL_MSG_MAX];
};

K_MSGQ_DEFINE(ol_msgq, sizeof(struct offline_log_record), OL_QUEUE_SIZE, 4);

/* fstab entry declared in the board DTS (node label sd_mp). */
FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(sd_mp));
static struct fs_mount_t *sd_mount = &FS_FSTAB_ENTRY(DT_NODELABEL(sd_mp));

static bool card_mounted;
/*
 * Set after the bounded mount-retry sequence is exhausted. Once disabled,
 * callers stop formatting/queueing events and the writer sleeps until reboot;
 * a missing or damaged card therefore cannot create a permanent 5 s retry loop.
 */
static atomic_t sd_disabled;
static int last_mount_error;
static bool file_open;
static bool pending_mode;    /* current file is under /olog/pending/ */
static bool time_trusted;
static bool sd_hold_for_download; /* writer file closed for remote download */
static uint32_t current_day_tag; /* YYYYMMDD of the open dated file */
static uint32_t boot_seq;        /* pending boot sequence */
static uint32_t day_seq;         /* sequence of the open dated file */
static char current_path[160];
static char hold_reopen_path[160];
static bool hold_pending_mode;
static uint32_t hold_day_tag;
static bool hold_reopen_valid;
static struct fs_file_t log_file;

static volatile uint32_t dropped_records;
static uint64_t total_bytes_written;
static uint64_t current_file_bytes;
static uint32_t dirs_removed;
static uint32_t pending_files_removed;
static uint32_t last_cleanup_day;
static uint32_t cleanup_tick_s;
static uint32_t space_check_tick_s;
static bool boot_confirm_logged;
static bool orphan_migrated;
static struct offline_log_record parked_rec;
static bool parked_rec_valid;

static K_MUTEX_DEFINE(sd_lock);

/* ------------------------------------------------------------------ */
/* small filesystem helpers                                            */
/* ------------------------------------------------------------------ */

static int ensure_dir(const char *path)
{
	struct fs_dirent ent;
	int rc;

	/* Zephyr fs_mkdir() logs -EEXIST as an error, so check existence first
	 * to keep the boot log clean for already-existing log directories.
	 */
	rc = fs_stat(path, &ent);
	if (rc == 0) {
		return 0;
	}

	rc = fs_mkdir(path);
	if (rc == -EEXIST) {
		return 0;
	}
	return rc;
}

static int ensure_dirs(void)
{
	char path[160];
	int rc;

	rc = snprintf(path, sizeof(path), "%s%s",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR);
	if (rc <= 0 || (size_t)rc >= sizeof(path)) {
		return -ENAMETOOLONG;
	}
	rc = ensure_dir(path);
	if (rc != 0) {
		return rc;
	}

	rc = snprintf(path, sizeof(path), "%s%s/pending",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR);
	if (rc <= 0 || (size_t)rc >= sizeof(path)) {
		return -ENAMETOOLONG;
	}
	return ensure_dir(path);
}

/*
 * Sequence parsers for this build's file names:
 *   <prefix>_pending_boot_<seq>.log
 *   <prefix>_<date>_<time>_<seq>.log
 * Requiring the prefix also keeps files of the other trolley variant out of
 * the numbering (each variant numbers its own files independently).
 */
static bool ol_parse_pending_seq(const char *name, uint32_t *seq)
{
	if (strncmp(name, OL_FILE_PREFIX "_", OL_FILE_PREFIX_LEN + 1U) != 0) {
		return false;
	}

	return sscanf(name + OL_FILE_PREFIX_LEN + 1U,
		      "pending_boot_%u.log", seq) == 1;
}

static bool ol_parse_dated_seq(const char *name, uint32_t *seq)
{
	const char *p;
	const char *us;

	if (strncmp(name, OL_FILE_PREFIX "_", OL_FILE_PREFIX_LEN + 1U) != 0) {
		return false;
	}

	p = name + OL_FILE_PREFIX_LEN + 1U;
	us = strrchr(p, '_');

	return us != NULL && sscanf(us + 1, "%u.log", seq) == 1;
}

static uint32_t find_next_pending_seq(void)
{
	char path[160];
	struct fs_dir_t dir;
	uint32_t seq = 1U;
	int rc;

	rc = snprintf(path, sizeof(path), "%s%s/pending",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR);
	if (rc <= 0 || (size_t)rc >= sizeof(path)) {
		return 1U;
	}

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, path) != 0) {
		return 1U;
	}

	for (;;) {
		struct fs_dirent ent;
		uint32_t n;

		if (fs_readdir(&dir, &ent) != 0 || ent.name[0] == '\0') {
			break;
		}
		if (ol_parse_pending_seq(ent.name, &n) && n >= seq) {
			seq = n + 1U;
		}
	}
	(void)fs_closedir(&dir);
	return seq;
}

static uint32_t find_next_day_seq(const char *day_dir)
{
	struct fs_dir_t dir;
	uint32_t seq = 1U;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, day_dir) != 0) {
		return 1U;
	}

	for (;;) {
		struct fs_dirent ent;
		uint32_t n;

		if (fs_readdir(&dir, &ent) != 0 || ent.name[0] == '\0') {
			break;
		}
		if (ol_parse_dated_seq(ent.name, &n) && n >= seq) {
			seq = n + 1U;
		}
	}
	(void)fs_closedir(&dir);
	return seq;
}

/* ------------------------------------------------------------------ */
/* date/time helpers                                                   */
/* ------------------------------------------------------------------ */

static int64_t days_from_civil(int y, int m, int d)
{
	int64_t era, yoe, doy, doe;

	y -= (m <= 2) ? 1 : 0;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void utc_ms_to_local_tm(int64_t utc_ms, struct tm *out)
{
	time_t local_s = (time_t)((utc_ms / 1000) +
		((int64_t)OL_TZ_OFFSET_MINUTES * 60));

	(void)gmtime_r(&local_s, out);
}

static void format_date_tag(const struct tm *tm, char *buf, size_t len)
{
	(void)snprintf(buf, len, "%04d-%02d-%02d",
		       tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

static void format_time_tag(const struct tm *tm, char *buf, size_t len)
{
	(void)snprintf(buf, len, "%02d-%02d-%02d",
		       tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static uint32_t compute_day_tag(const struct tm *tm)
{
	return (uint32_t)(tm->tm_year + 1900) * 10000U +
	       (uint32_t)(tm->tm_mon + 1) * 100U +
	       (uint32_t)tm->tm_mday;
}

/* Strict YYYY-MM-DD check. Returns true and day-number for valid names. */
static bool parse_date_dir(const char *name, int64_t *day_num)
{
	int y, m, d;
	unsigned int consumed;

	if (strlen(name) != 10U || name[4] != '-' || name[7] != '-') {
		return false;
	}
	if (sscanf(name, "%4d-%2d-%2d%n", &y, &m, &d, &consumed) != 3 ||
	    consumed != 10U) {
		return false;
	}
	if (y < 2000 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31) {
		return false;
	}
	*day_num = days_from_civil(y, m, d);
	return true;
}

static int format_time_prefix(char *buf, size_t len)
{
#if defined(CONFIG_TIME_MANAGER_SERVICE)
	int64_t utc_ms;
	char base[40];

	if (time_trusted && time_manager_get_utc_ms(&utc_ms) == 0) {
		int rc;

		rc = time_manager_format_local_iso8601_from_utc_ms(
			utc_ms, base, sizeof(base));
		if (rc == 0 && strlen(base) >= 20U) {
			/* Insert ".mmm" between seconds and timezone sign. */
			unsigned int ms = (unsigned int)(utc_ms % 1000);
			int n = snprintf(buf, len, "[%.19s.%03u%s] ",
					 base, ms, &base[19]);
			return (n > 0 && (size_t)n < len) ? 0 : -ENOMEM;
		}
	}
#endif
	{
		int n = snprintf(buf, len, "[uptime %" PRIu32 " ms] ",
				 k_uptime_get_32());
		return (n > 0 && (size_t)n < len) ? 0 : -ENOMEM;
	}
}

/* ------------------------------------------------------------------ */
/* file open/close/migrate                                             */
/* ------------------------------------------------------------------ */

static int write_header_to_open_file(const char *path)
{
	char hdr[256];
	int n = 0;

	n += snprintf(&hdr[n], sizeof(hdr) - (size_t)n,
		      "# offline log v1 boot_ms=%" PRIu32 "\n",
		      k_uptime_get_32());
#if defined(CONFIG_DEVICE_IDENTITY_SERVICE)
	{
		const struct device_identity *id = device_identity_get_info();

		if (id != NULL) {
			n += snprintf(&hdr[n], sizeof(hdr) - (size_t)n,
				      "# device product=%s model=%s host=%s "
				      "uid=%s short=%s\n",
				      id->product, id->model, id->hostname,
				      id->hardware_uid, id->short_id);
		}
	}
#endif
	n += snprintf(&hdr[n], sizeof(hdr) - (size_t)n,
		      "# fw app=%d.%d.%d board=%s\n",
		      APP_VERSION_MAJOR, APP_VERSION_MINOR,
		      APP_PATCHLEVEL, CONFIG_BOARD);

	if (n <= 0 || (size_t)n >= sizeof(hdr)) {
		return -ENOMEM;
	}
	if (fs_write(&log_file, hdr, (size_t)n) != (ssize_t)n) {
		return -EIO;
	}
	total_bytes_written += (uint64_t)n;
	current_file_bytes += (uint64_t)n;
	return 0;
}

static int open_pending_log(void)
{
	char dir[128];
	char path[160];
	int rc;

	rc = snprintf(dir, sizeof(dir), "%s%s/pending",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR);
	if (rc <= 0 || (size_t)rc >= sizeof(dir)) {
		return -ENAMETOOLONG;
	}
	rc = ensure_dir(dir);
	if (rc != 0) {
		return rc;
	}

	/* Pick the next free sequence every time a pending file is opened:
	 * power cycles (and pending file size rotation) must never overwrite
	 * or append into an older boot's pending file. */
	boot_seq = find_next_pending_seq();
	rc = snprintf(path, sizeof(path),
		      "%s/" OL_FILE_PREFIX "_pending_boot_%06u.log",
		      dir, boot_seq);
	if (rc <= 0 || (size_t)rc >= sizeof(path)) {
		return -ENAMETOOLONG;
	}

	fs_file_t_init(&log_file);
	rc = fs_open(&log_file, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc != 0) {
		return rc;
	}
	file_open = true;
	pending_mode = true;
	current_file_bytes = 0U;
	(void)snprintf(current_path, sizeof(current_path), "%s", path);

	rc = write_header_to_open_file(path);
	if (rc != 0) {
		(void)fs_close(&log_file);
		file_open = false;
		(void)fs_unlink(path);
		return rc;
	}
	return 0;
}

static void write_time_sync_banner(void)
{
#if defined(CONFIG_TIME_MANAGER_SERVICE)
	char line[192];
	int64_t utc_ms;
	int n;

	if (time_manager_get_utc_ms(&utc_ms) != 0) {
		return;
	}
	n = snprintf(line, sizeof(line),
		     "--- TIME_SYNC established unix_ms=%" PRId64
		     " source=%s ---\n",
		     utc_ms,
		     time_manager_source_name(time_manager_get_active_source()));
	if (n > 0 && (size_t)n < sizeof(line)) {
		if (fs_write(&log_file, line, (size_t)n) == (ssize_t)n) {
			total_bytes_written += (uint64_t)n;
			current_file_bytes += (uint64_t)n;
		}
	}
#endif
	(void)fs_sync(&log_file);
}

static int open_dated_log(void)
{
#if defined(CONFIG_TIME_MANAGER_SERVICE)
	char date_tag[32];
	char time_tag[16];
	char day_dir[128];
	char path[176];
	struct tm tm;
	int64_t utc_ms;
	uint32_t seq;
	int rc;

	if (time_manager_get_utc_ms(&utc_ms) != 0) {
		return -ENODATA;
	}
	utc_ms_to_local_tm(utc_ms, &tm);
	format_date_tag(&tm, date_tag, sizeof(date_tag));
	format_time_tag(&tm, time_tag, sizeof(time_tag));

	rc = snprintf(day_dir, sizeof(day_dir), "%s%s/%s",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR,
		      date_tag);
	if (rc <= 0 || (size_t)rc >= sizeof(day_dir)) {
		return -ENAMETOOLONG;
	}
	rc = ensure_dir(day_dir);
	if (rc != 0) {
		return rc;
	}

	seq = find_next_day_seq(day_dir);
	rc = snprintf(path, sizeof(path),
		      "%s/" OL_FILE_PREFIX "_%s_%s_%06u.log",
		      day_dir, date_tag, time_tag, seq);
	if (rc <= 0 || (size_t)rc >= sizeof(path)) {
		return -ENAMETOOLONG;
	}

	fs_file_t_init(&log_file);
	rc = fs_open(&log_file, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc != 0) {
		return rc;
	}
	file_open = true;
	pending_mode = false;
	time_trusted = true;
	current_day_tag = compute_day_tag(&tm);
	day_seq = seq;
	current_file_bytes = 0U;
	(void)snprintf(current_path, sizeof(current_path), "%s", path);

	rc = write_header_to_open_file(path);
	if (rc != 0) {
		(void)fs_close(&log_file);
		file_open = false;
		(void)fs_unlink(path);
		return rc;
	}
	write_time_sync_banner();
	return 0;
#else
	return -ENOTSUP;
#endif
}

static int migrate_pending_to_dated(void)
{
#if defined(CONFIG_TIME_MANAGER_SERVICE)
	char date_tag[16];
	char day_dir[128];
	char new_path[176];
	char base_name[64];
	const char *slash;
	struct tm tm;
	int64_t utc_ms;
	int rc;

	if (!file_open || !pending_mode ||
	    time_manager_get_utc_ms(&utc_ms) != 0) {
		return 0;
	}
	utc_ms_to_local_tm(utc_ms, &tm);
	format_date_tag(&tm, date_tag, sizeof(date_tag));

	write_time_sync_banner();
	(void)fs_close(&log_file);
	file_open = false;

	/* Keep the original <prefix>_pending_boot_*.log name so the file
	 * remains identifiable as having been written before a trusted time
	 * source was available. */
	slash = strrchr(current_path, '/');
	(void)snprintf(base_name, sizeof(base_name), "%s",
		       slash != NULL ? slash + 1 : current_path);

	rc = snprintf(day_dir, sizeof(day_dir), "%s%s/%s",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR,
		      date_tag);
	if (rc <= 0 || (size_t)rc >= sizeof(day_dir)) {
		return -ENAMETOOLONG;
	}
	rc = ensure_dir(day_dir);
	if (rc != 0) {
		return rc;
	}

	rc = snprintf(new_path, sizeof(new_path), "%s/%s",
		      day_dir, base_name);
	if (rc <= 0 || (size_t)rc >= sizeof(new_path)) {
		return -ENAMETOOLONG;
	}

	rc = fs_rename(current_path, new_path);
	if (rc != 0) {
		LOG_ERR("olog rename %s -> %s failed: %d",
			current_path, new_path, rc);
		return rc;
	}

	time_trusted = true;
	current_day_tag = compute_day_tag(&tm);
	pending_mode = false;
	current_path[0] = '\0';
	LOG_INF("olog: pending log migrated to %s", new_path);
	return 0;
#else
	return 0;
#endif
}

/*
 * A previous boot may have left <prefix>_pending_boot_*.log files when the
 * device rebooted before any trusted time source was available. Once we have
 * trusted time, rename every remaining pending file into today's dated
 * directory so the early-boot events are not lost.
 */
static void migrate_orphan_pending_locked(void)
{
#if defined(CONFIG_TIME_MANAGER_SERVICE)
	char pending_dir[128];
	char day_dir[128];
	char date_tag[16];
	struct fs_dir_t dir;
	struct tm tm;
	int64_t utc_ms;
	int rc;

	if (!time_trusted || time_manager_get_utc_ms(&utc_ms) != 0) {
		return;
	}

	rc = snprintf(pending_dir, sizeof(pending_dir), "%s%s/pending",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR);
	if (rc <= 0 || (size_t)rc >= sizeof(pending_dir)) {
		return;
	}
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, pending_dir) != 0) {
		return;
	}

	utc_ms_to_local_tm(utc_ms, &tm);
	format_date_tag(&tm, date_tag, sizeof(date_tag));
	rc = snprintf(day_dir, sizeof(day_dir), "%s%s/%s",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR,
		      date_tag);
	if (rc <= 0 || (size_t)rc >= sizeof(day_dir)) {
		(void)fs_closedir(&dir);
		return;
	}
	if (ensure_dir(day_dir) != 0) {
		(void)fs_closedir(&dir);
		return;
	}

	for (;;) {
		struct fs_dirent ent;
		char old_path[176];
		char new_path[192];
		uint32_t n;

		if (fs_readdir(&dir, &ent) != 0 || ent.name[0] == '\0') {
			break;
		}
		if (!ol_parse_pending_seq(ent.name, &n)) {
			continue;
		}
		(void)snprintf(old_path, sizeof(old_path), "%s/%s",
			       pending_dir, ent.name);
		/* Keep the <prefix>_pending_boot_*.log filename during
		 * migration so the file stays identifiable. */
		(void)snprintf(new_path, sizeof(new_path), "%s/%s",
			       day_dir, ent.name);
		if (fs_rename(old_path, new_path) == 0) {
			LOG_INF("olog: migrated orphan pending %s -> %s",
				old_path, new_path);
		}
	}
	(void)fs_closedir(&dir);
#endif
}

static void close_log_file(void)
{
	if (file_open) {
		(void)fs_sync(&log_file);
		(void)fs_close(&log_file);
	}
	file_open = false;
	pending_mode = false;
	current_path[0] = '\0';
}

/*
 * Called by the MCUmgr file-access hook before a remote download starts.
 * If the requested file is exactly the log file currently open for appending,
 * close it (after any in-flight write finishes) so the download reads a
 * stable snapshot. The writer thread is paused while the file stays closed
 * and events accumulate in the RAM queue.
 *
 * Returns 0 when the file was held/closed, 1 when the path is not the current
 * open file (nothing to do), or a negative error.
 */
int offline_log_sd_hold_open_file_for_read(const char *path)
{
	int rc;

	if (path == NULL) {
		return -EINVAL;
	}
	if (!offline_log_sd_is_enabled()) {
		return -ENODEV;
	}

	k_mutex_lock(&sd_lock, K_FOREVER);
	if (card_mounted && file_open &&
	    strcmp(current_path, path) == 0) {
		/* Remember where to resume after the download finishes. */
		(void)snprintf(hold_reopen_path,
			       sizeof(hold_reopen_path), "%s",
			       current_path);
		hold_pending_mode = pending_mode;
		hold_day_tag = current_day_tag;
		hold_reopen_valid = true;
		close_log_file();
		sd_hold_for_download = true;
		rc = 0;
	} else {
		rc = 1;
	}
	k_mutex_unlock(&sd_lock);

	return rc;
}

/* Re-open the file that was closed for download (append mode, without
 * rewriting the header) so the writer continues in the same file. */
static void reopen_after_hold_locked(void)
{
	struct fs_dirent ent;

	if (!hold_reopen_valid || hold_reopen_path[0] == '\0' ||
	    !card_mounted) {
		hold_reopen_valid = false;
		return;
	}

	fs_file_t_init(&log_file);
	if (fs_open(&log_file, hold_reopen_path,
		    FS_O_WRITE | FS_O_APPEND) != 0) {
		LOG_WRN("olog: reopen held file failed, a new file will be "
			"started on the next record");
		hold_reopen_valid = false;
		return;
	}

	file_open = true;
	pending_mode = hold_pending_mode;
	time_trusted = !hold_pending_mode;
	current_day_tag = hold_day_tag;
	current_file_bytes = 0U;
	if (fs_stat(hold_reopen_path, &ent) == 0) {
		current_file_bytes = ent.size;
	}
	(void)snprintf(current_path, sizeof(current_path), "%s",
		       hold_reopen_path);
	hold_reopen_valid = false;
	LOG_INF("olog: resumed appending to %s", current_path);
}

/* Resume the writer after the remote file transfer finished (or was closed
 * by the MCUmgr idle timeout): reopen the same file in append mode. */
void offline_log_sd_release_after_read(void)
{
	k_mutex_lock(&sd_lock, K_FOREVER);
	sd_hold_for_download = false;
	reopen_after_hold_locked();
	k_mutex_unlock(&sd_lock);
}

/* ------------------------------------------------------------------ */
/* record writing                                                      */
/* ------------------------------------------------------------------ */

static int write_record_locked(const struct offline_log_record *rec)
{
	char prefix[48];
	char line[OL_MSG_MAX + 96];
	ssize_t wr;
	int n;

	if (!card_mounted) {
		return -ENODEV;
	}
	if (!file_open) {
		int rc = time_trusted ? open_dated_log() : open_pending_log();

		if (rc != 0) {
			return rc;
		}
	}

	(void)format_time_prefix(prefix, sizeof(prefix));
	n = snprintf(line, sizeof(line), "%s<%s> [%s] %s\n", prefix,
		     offline_log_level_name((enum offline_log_level)rec->level),
		     offline_log_category_name(
			     (enum offline_log_category)rec->category),
		     rec->text);
	if (n <= 0 || (size_t)n >= sizeof(line)) {
		return -ENOMEM;
	}

	wr = fs_write(&log_file, line, (size_t)n);
	if (wr != (ssize_t)n) {
		return -EIO;
	}
	total_bytes_written += (uint64_t)n;
	current_file_bytes += (uint64_t)n;

	if (fs_sync(&log_file) != 0) {
		return -EIO;
	}

	if (current_file_bytes >= OL_MAX_FILE_BYTES) {
		/* Close; the next record lazily opens a new daily file. */
		close_log_file();
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* retention / size cleanup                                            */
/* ------------------------------------------------------------------ */

static uint64_t dir_total_size_locked(const char *dir_path)
{
	struct fs_dir_t dir;
	uint64_t total = 0U;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, dir_path) != 0) {
		return 0U;
	}
	for (;;) {
		struct fs_dirent ent;
		char sub[192];

		if (fs_readdir(&dir, &ent) != 0 || ent.name[0] == '\0') {
			break;
		}
		if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) {
			continue;
		}
		(void)snprintf(sub, sizeof(sub), "%s/%s", dir_path, ent.name);
		if (ent.type == FS_DIR_ENTRY_DIR) {
			total += dir_total_size_locked(sub);
		} else {
			total += (uint64_t)ent.size;
		}
	}
	(void)fs_closedir(&dir);
	return total;
}

static int delete_dir_recursive_locked(const char *path)
{
	struct fs_dir_t dir;
	int rc;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, path) != 0) {
		return fs_unlink(path);
	}
	rc = 0;
	for (;;) {
		struct fs_dirent ent;
		char sub[192];

		if (fs_readdir(&dir, &ent) != 0 || ent.name[0] == '\0') {
			break;
		}
		if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) {
			continue;
		}
		(void)snprintf(sub, sizeof(sub), "%s/%s", path, ent.name);
		if (ent.type == FS_DIR_ENTRY_DIR) {
			rc = delete_dir_recursive_locked(sub);
		} else {
			rc = fs_unlink(sub);
		}
		if (rc != 0) {
			break;
		}
	}
	(void)fs_closedir(&dir);
	if (rc == 0) {
		rc = fs_unlink(path);
	}
	return rc;
}

struct ol_date_dir {
	char name[16];
	int64_t day_num;
	uint64_t size;
};

/*
 * Cleanup workspace. A 64-entry date-directory table is too large for the
 * low-priority writer stack, so it is kept static and placed in DTCM to avoid
 * consuming AXI SRAM. All cleanup paths run under sd_lock, so this buffer is
 * never used concurrently.
 */
static struct ol_date_dir cleanup_date_dirs[64] __dtcm_noinit_section;

static int collect_date_dirs_locked(const char *root,
				    struct ol_date_dir *dirs,
				    size_t max, size_t *count)
{
	struct fs_dir_t dir;
	size_t n = 0U;

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, root) != 0) {
		*count = 0U;
		return 0;
	}
	for (;;) {
		struct fs_dirent ent;
		char full[176];
		int64_t day_num;

		if (fs_readdir(&dir, &ent) != 0 || ent.name[0] == '\0') {
			break;
		}
		if (ent.type != FS_DIR_ENTRY_DIR ||
		    !parse_date_dir(ent.name, &day_num) || n >= max) {
			continue;
		}
		(void)snprintf(full, sizeof(full), "%s/%s", root, ent.name);
		(void)snprintf(dirs[n].name, sizeof(dirs[n].name), "%s",
			       ent.name);
		dirs[n].day_num = day_num;
		dirs[n].size = dir_total_size_locked(full);
		n++;
	}
	(void)fs_closedir(&dir);
	*count = n;
	return 0;
}

static void sort_date_dirs(struct ol_date_dir *dirs, size_t count)
{
	for (size_t i = 1U; i < count; i++) {
		struct ol_date_dir key = dirs[i];
		size_t j = i;

		while (j > 0U && strcmp(dirs[j - 1U].name, key.name) > 0) {
			dirs[j] = dirs[j - 1U];
			j--;
		}
		dirs[j] = key;
	}
}

/* Returns free/total space in KB through fs_statvfs. */
static int sd_space_kb(uint64_t *free_kb, uint64_t *total_kb)
{
	struct fs_statvfs st;
	uint64_t frsize;

	if (free_kb == NULL || total_kb == NULL) {
		return -EINVAL;
	}
	if (fs_statvfs(sd_mount->mnt_point, &st) != 0) {
		return -EIO;
	}
	frsize = st.f_frsize != 0U ? (uint64_t)st.f_frsize :
				     (uint64_t)st.f_bsize;
	*total_kb = ((uint64_t)st.f_blocks * frsize) / 1024U;
	*free_kb = ((uint64_t)st.f_bfree * frsize) / 1024U;
	return 0;
}

/* Space floor below which cleanup deletes old logs:
 * max(absolute KB, percent of total capacity). */
static uint64_t space_floor_kb(uint64_t total_kb)
{
	uint64_t percent_kb = ((uint64_t)OL_FREE_MIN_PERCENT * total_kb) / 100U;

	return percent_kb > (uint64_t)OL_FREE_MIN_KB ?
	       percent_kb : (uint64_t)OL_FREE_MIN_KB;
}

/* Record a deletion in the offline log itself (queued, never blocks). */
static void log_cleanup_delete(const char *date_dir, const char *reason,
			       uint64_t size_kb)
{
	char text[192];

	(void)snprintf(text, sizeof(text),
		       "offline log cleanup removed date_dir=%s reason=%s "
		       "bytes_kb=%llu",
		       date_dir, reason, (unsigned long long)size_kb);
	(void)offline_log_sd_enqueue(OFFLINE_LOG_CAT_SYSTEM,
				     OFFLINE_LOG_LEVEL_INF, text);
}

/* Delete one date dir and log the result. Returns 0 on success. */
static int delete_one_date_dir_locked(const char *root,
				      const struct ol_date_dir *d,
				      const char *reason)
{
	char full[176];
	int rc;

	(void)snprintf(full, sizeof(full), "%s/%s", root, d->name);
	rc = delete_dir_recursive_locked(full);
	if (rc == 0) {
		dirs_removed++;
		LOG_WRN("olog: %s cleanup removed %s", reason, full);
		log_cleanup_delete(d->name, reason, d->size / 1024U);
	} else {
		LOG_ERR("olog: %s cleanup failed to remove %s: %d",
			reason, full, rc);
		(void)offline_log_sd_enqueue(OFFLINE_LOG_CAT_SYSTEM,
					     OFFLINE_LOG_LEVEL_ERR, full);
	}
	return rc;
}

/*
 * Capacity guard for the pending directory while no trusted time exists.
 * Date retention does not cover <prefix>_pending_boot_*.log, so remove the
 * oldest files when the pending directory exceeds its own cap. The open
 * file is skipped, and at most 32 files are removed per pass to keep the
 * writer thread stall short.
 */
static int pending_cap_cleanup_locked(void)
{
	char pending_dir[128];
	uint32_t removed = 0U;
	int rc;

	if (!card_mounted) {
		return 0;
	}

	rc = snprintf(pending_dir, sizeof(pending_dir), "%s%s/pending",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR);
	if (rc <= 0 || (size_t)rc >= sizeof(pending_dir)) {
		return 0;
	}
	if (ensure_dir(pending_dir) != 0) {
		return 0;
	}

	for (;;) {
		struct fs_dir_t dir;
		uint64_t total = 0U;
		char oldest_path[176];
		uint32_t oldest_seq = 0U;
		uint64_t oldest_size = 0U;
		bool have_oldest = false;

		fs_dir_t_init(&dir);
		if (fs_opendir(&dir, pending_dir) != 0) {
			return 0;
		}

		for (;;) {
			struct fs_dirent ent;
			char full[176];
			uint32_t seq;

			if (fs_readdir(&dir, &ent) != 0 ||
			    ent.name[0] == '\0') {
				break;
			}
			if (!ol_parse_pending_seq(ent.name, &seq)) {
				continue;
			}
			(void)snprintf(full, sizeof(full), "%s/%s",
				       pending_dir, ent.name);
			/* Never delete the file that is open for appending. */
			if (file_open && strcmp(full, current_path) == 0) {
				continue;
			}

			total += ent.size;
			if (!have_oldest || seq < oldest_seq) {
				have_oldest = true;
				oldest_seq = seq;
				oldest_size = ent.size;
				(void)snprintf(oldest_path,
					       sizeof(oldest_path), "%s",
					       full);
			}
		}
		(void)fs_closedir(&dir);

		if (total <= OL_PENDING_MAX_TOTAL_BYTES || !have_oldest) {
			if (total > OL_PENDING_MAX_TOTAL_BYTES) {
				LOG_WRN("olog: pending over cap (%llu KB) but "
					"no removable pending file",
					(unsigned long long)(total / 1024U));
			}
			return 0;
		}

		rc = fs_unlink(oldest_path);
		if (rc != 0) {
			LOG_ERR("olog: pending cleanup failed to remove "
				"%s: %d", oldest_path, rc);
			return 0;
		}
		pending_files_removed++;
		LOG_WRN("olog: pending cap cleanup removed %s", oldest_path);
		{
			char text[192];

			(void)snprintf(text, sizeof(text),
				       "offline log cleanup removed "
				       "pending_file=%s reason=pending_cap "
				       "bytes_kb=%llu",
				       oldest_path,
				       (unsigned long long)
				       (oldest_size / 1024U));
			(void)offline_log_sd_enqueue(OFFLINE_LOG_CAT_SYSTEM,
						     OFFLINE_LOG_LEVEL_WRN,
						     text);
		}

		removed++;
		if (removed >= OL_PENDING_CLEANUP_MAX_FILES) {
			return 0;
		}
	}
}

static int cleanup_locked(void)
{
	char root[160];
	size_t count = 0U;
	size_t removed = 0U;
	uint64_t total = 0U;
	uint64_t free_kb = 0U;
	uint64_t total_kb = 0U;
	int rc;

	rc = snprintf(root, sizeof(root), "%s%s",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR);
	if (rc <= 0 || (size_t)rc >= sizeof(root)) {
		return -ENAMETOOLONG;
	}
	if (ensure_dir(root) != 0) {
		return -ENODEV;
	}

	/* 1) retention by day count (only when we know "today") */
#if defined(CONFIG_TIME_MANAGER_SERVICE)
	if (time_trusted) {
		struct tm tm;
		int64_t utc_ms;
		char today_tag[16];
		int64_t today_num = -1;

		if (time_manager_get_utc_ms(&utc_ms) == 0) {
			utc_ms_to_local_tm(utc_ms, &tm);
			(void)snprintf(today_tag, sizeof(today_tag),
				       "%04d-%02d-%02d",
				       tm.tm_year + 1900, tm.tm_mon + 1,
				       tm.tm_mday);
			(void)parse_date_dir(today_tag, &today_num);
		}
		if (today_num >= 0) {
			collect_date_dirs_locked(root, cleanup_date_dirs,
						 ARRAY_SIZE(cleanup_date_dirs),
						 &count);
			sort_date_dirs(cleanup_date_dirs, count);
			for (size_t i = 0U; i < count &&
			     removed < OL_CLEANUP_MAX_DIRS; i++) {
				if (cleanup_date_dirs[i].day_num >=
				    today_num - (int64_t)OL_RETENTION_DAYS) {
					continue;
				}
				if (delete_one_date_dir_locked(root,
							       &cleanup_date_dirs[i],
							       "retention") == 0) {
					removed++;
				}
			}
		}
	}
#endif

	/* 2) size cap + free-space floor: delete oldest date dirs first */
	collect_date_dirs_locked(root, cleanup_date_dirs,
				 ARRAY_SIZE(cleanup_date_dirs), &count);
	sort_date_dirs(cleanup_date_dirs, count);
	total = 0U;
	for (size_t i = 0U; i < count; i++) {
		total += cleanup_date_dirs[i].size;
	}
	if (sd_space_kb(&free_kb, &total_kb) != 0) {
		free_kb = 0U;
		total_kb = 0U;
	}

	for (size_t i = 0U; i < count &&
	     removed < OL_CLEANUP_MAX_DIRS; i++) {
		const char *reason = NULL;

		if (total > OL_MAX_TOTAL_BYTES) {
			reason = "size_cap";
		} else if (total_kb != 0U &&
			   free_kb < space_floor_kb(total_kb)) {
			reason = "space";
		} else {
			break;
		}

		if (delete_one_date_dir_locked(root, &cleanup_date_dirs[i],
					       reason) != 0) {
			continue; /* skip to next oldest dir on failure */
		}
		removed++;
		if (total >= cleanup_date_dirs[i].size) {
			total -= cleanup_date_dirs[i].size;
		} else {
			total = 0U;
		}
		if (sd_space_kb(&free_kb, &total_kb) != 0) {
			free_kb = 0U;
			total_kb = 0U;
		}
	}

	if (removed == 0U && total_kb != 0U &&
	    free_kb < space_floor_kb(total_kb)) {
		LOG_WRN("olog: free space low (%llu KB) but no older date "
			"dirs left to remove", (unsigned long long)free_kb);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* maintenance tick                                                    */
/* ------------------------------------------------------------------ */

static void maintenance_locked(void)
{
	bool run_cleanup = false;
	uint32_t today_tag = 0U;

#if defined(CONFIG_TIME_MANAGER_SERVICE)
	struct tm tm;
	int64_t utc_ms;

	if (time_manager_has_trusted_time() && !time_trusted) {
		time_trusted = true;
		if (file_open && pending_mode) {
			(void)migrate_pending_to_dated();
		}
		if (!orphan_migrated) {
			orphan_migrated = true;
			migrate_orphan_pending_locked();
		}
		return;
	}

	if (time_trusted && !orphan_migrated) {
		orphan_migrated = true;
		migrate_orphan_pending_locked();
	}

	if (time_trusted) {
		if (time_manager_get_utc_ms(&utc_ms) != 0) {
			return;
		}
		utc_ms_to_local_tm(utc_ms, &tm);
		if (file_open && !pending_mode &&
		    compute_day_tag(&tm) != current_day_tag) {
			close_log_file();
		}
		today_tag = compute_day_tag(&tm);
	}
#endif

	/* Periodic cleanup: hourly, once per new day, or when the SD free
	 * space drops below the configured floor (checked every 5 minutes).
	 */
	cleanup_tick_s++;
	space_check_tick_s++;
	if (cleanup_tick_s >= OL_CLEANUP_INTERVAL_S ||
	    (time_trusted && last_cleanup_day != today_tag)) {
		run_cleanup = true;
	} else if (space_check_tick_s >= OL_SPACE_CHECK_INTERVAL_S) {
		uint64_t free_kb;
		uint64_t total_kb;

		space_check_tick_s = 0U;
		if (card_mounted && sd_space_kb(&free_kb, &total_kb) == 0 &&
		    total_kb != 0U &&
		    free_kb < space_floor_kb(total_kb)) {
			run_cleanup = true;
		}
	}
	if (run_cleanup && card_mounted) {
		cleanup_tick_s = 0U;
#if defined(CONFIG_TIME_MANAGER_SERVICE)
		if (time_trusted) {
			last_cleanup_day = today_tag;
			(void)cleanup_locked();
		} else
#endif
		{
			/* No trusted time: date retention does not apply, so
			 * enforce the independent pending-directory cap. */
			(void)pending_cap_cleanup_locked();
		}
	}

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	/* Detect "image confirm" issued through shell/OTA service. */
	if (!boot_confirm_logged && boot_is_img_confirmed()) {
		boot_confirm_logged = true;
		offline_log_sd_enqueue(OFFLINE_LOG_CAT_OTA,
				       OFFLINE_LOG_LEVEL_INF,
				       "OTA image confirmed");
	}
#endif
}

/* ------------------------------------------------------------------ */
/* mount / writer thread                                               */
/* ------------------------------------------------------------------ */

bool offline_log_sd_is_enabled(void)
{
	return atomic_get(&sd_disabled) == 0;
}

/*
 * Return the driver and HAL to a clean state before another mount attempt.
 * A failed HAL_SD_Init() is not a complete deinitialization, so retrying
 * disk_access_init() directly can keep the controller stuck in its failed
 * power-on state. Force deinit also handles a disk that was initialized but
 * whose filesystem mount failed.
 */
static void reset_disk_locked(void)
{
	bool force = true;
	int rc;

	close_log_file();
	sd_hold_for_download = false;
	hold_reopen_valid = false;

	if (card_mounted) {
		rc = fs_unmount(sd_mount);
		if (rc != 0) {
			LOG_WRN("olog: SD unmount during reset failed: %d", rc);
		}
	}
	card_mounted = false;

	rc = disk_access_ioctl(CONFIG_OFFLINE_LOG_DISK_NAME,
			       DISK_IOCTL_CTRL_DEINIT, &force);
	if (rc != 0) {
		LOG_DBG("olog: SD force deinit returned %d", rc);
	}
	k_sleep(K_MSEC(OL_DISK_RESET_SETTLE_MS));
}

static void disable_backend_locked(int last_error)
{
	uint32_t pending = k_msgq_num_used_get(&ol_msgq);

	atomic_set(&sd_disabled, 1);
	last_mount_error = last_error;
	close_log_file();
	card_mounted = false;
	sd_hold_for_download = false;
	hold_reopen_valid = false;
	parked_rec_valid = false;

	if (pending != 0U) {
		dropped_records += pending;
		k_msgq_purge(&ol_msgq);
	}

	LOG_ERR("olog: offline log disabled after %u mount attempts "
		"(last error %d, dropped %u queued record(s))",
		(unsigned int)OL_MOUNT_MAX_ATTEMPTS, last_error,
		pending);
}

static int try_mount_once(void)
{
	struct fs_statvfs stats;
	int rc;

	rc = disk_access_init(CONFIG_OFFLINE_LOG_DISK_NAME);
	if (rc != 0) {
		return rc;
	}

	/* Give a card inserted after power-on time to finish its internal
	 * power-on reset and mechanical-contact settling before FATFS reads
	 * the volume.
	 */
	k_sleep(K_MSEC(OL_CARD_SETTLE_MS));

	rc = fs_mount(sd_mount);
	if (rc == -EBUSY) {
		/*
		 * Defensive recovery for a stale FATFS mount. The normal reset
		 * path unmounts successfully, but if the mounted-state flag and
		 * the filesystem registry ever disagree, do not let every later
		 * attempt fail permanently with "file system already mounted".
		 */
		(void)fs_unmount(sd_mount);
		rc = fs_mount(sd_mount);
	}
	if (rc != 0) {
		return rc;
	}
	rc = fs_statvfs(sd_mount->mnt_point, &stats);
	if (rc != 0) {
		(void)fs_unmount(sd_mount);
		return rc;
	}
	card_mounted = true;
	rc = ensure_dirs();
	if (rc != 0) {
		LOG_ERR("olog: prepare dirs failed: %d", rc);
		(void)fs_unmount(sd_mount);
		card_mounted = false;
		return rc;
	}
	last_mount_error = 0;
	LOG_INF("olog: SD card mounted at %s", sd_mount->mnt_point);
	return 0;
}

static int mount_with_bounded_retries(void)
{
	int rc = -ENODEV;

	for (uint32_t attempt = 1U; attempt <= OL_MOUNT_MAX_ATTEMPTS;
	     attempt++) {
		rc = try_mount_once();
		if (rc == 0) {
			return 0;
		}

		last_mount_error = rc;
		LOG_WRN("olog: SD mount attempt %u/%u failed: %d",
			attempt, (unsigned int)OL_MOUNT_MAX_ATTEMPTS, rc);
		reset_disk_locked();

		if (attempt < OL_MOUNT_MAX_ATTEMPTS) {
			k_sleep(K_MSEC(CONFIG_OFFLINE_LOG_MOUNT_RETRY_MS));
		}
	}

	disable_backend_locked(rc);
	return rc;
}

K_THREAD_STACK_DEFINE(ol_writer_stack, CONFIG_OFFLINE_LOG_WRITER_STACK_SIZE);
static struct k_thread ol_writer_thread;

static void ol_writer_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct offline_log_record rec;
		int rc;

		if (!offline_log_sd_is_enabled()) {
			k_sleep(K_FOREVER);
		}

		if (!card_mounted) {
			k_mutex_lock(&sd_lock, K_FOREVER);
			rc = mount_with_bounded_retries();
			k_mutex_unlock(&sd_lock);
			if (rc != 0) {
				continue;
			}
		}

		k_mutex_lock(&sd_lock, K_FOREVER);
		if (sd_hold_for_download) {
			/* Remote download holds the current file closed; keep
			 * records in the RAM queue (or parked) until release.
			 */
			k_mutex_unlock(&sd_lock);
			k_sleep(K_MSEC(50));
			continue;
		}

		maintenance_locked();
		if (card_mounted &&
		    disk_access_status(CONFIG_OFFLINE_LOG_DISK_NAME) != 0) {
			LOG_WRN("olog: SD media/controller error, remounting");
			reset_disk_locked();
			k_mutex_unlock(&sd_lock);
			continue;
		}

		if (parked_rec_valid) {
			/* A record dequeued just before the download hold was
			 * parked here to preserve ordering. */
			rec = parked_rec;
			parked_rec_valid = false;
			if (write_record_locked(&rec) != 0) {
				LOG_ERR("olog: write failed, card may be gone");
				reset_disk_locked();
			}
			k_mutex_unlock(&sd_lock);
			continue;
		}
		k_mutex_unlock(&sd_lock);

		rc = k_msgq_get(&ol_msgq, &rec, K_MSEC(1000));
		if (rc != 0) {
			continue;
		}

		k_mutex_lock(&sd_lock, K_FOREVER);
		if (sd_hold_for_download) {
			/* Hold started between the check and the dequeue;
			 * park this record and wait for the release. */
			parked_rec = rec;
			parked_rec_valid = true;
			k_mutex_unlock(&sd_lock);
			continue;
		}
		if (write_record_locked(&rec) != 0) {
			LOG_ERR("olog: write failed, card may be gone");
			reset_disk_locked();
		}
		k_mutex_unlock(&sd_lock);
	}
}

int offline_log_sd_start(void)
{
	atomic_clear(&sd_disabled);
	last_mount_error = 0;
#if defined(CONFIG_TIME_MANAGER_SERVICE)
	time_trusted = time_manager_has_trusted_time();
#endif
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	boot_confirm_logged = boot_is_img_confirmed();
#endif
	k_thread_create(&ol_writer_thread, ol_writer_stack,
			K_THREAD_STACK_SIZEOF(ol_writer_stack),
			ol_writer_fn, NULL, NULL, NULL,
			CONFIG_OFFLINE_LOG_WRITER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&ol_writer_thread, "olog_writer");
	return 0;
}

int offline_log_sd_enqueue(enum offline_log_category cat,
			   enum offline_log_level level,
			   const char *text)
{
	struct offline_log_record rec;

	if (text == NULL) {
		return -EINVAL;
	}
	if (!offline_log_sd_is_enabled()) {
		return -ENODEV;
	}
	rec.level = (uint8_t)level;
	rec.category = (uint8_t)cat;
	(void)snprintf(rec.text, sizeof(rec.text), "%s", text);

	while (k_msgq_put(&ol_msgq, &rec, K_NO_WAIT) != 0) {
		struct offline_log_record discard;

		if (k_msgq_get(&ol_msgq, &discard, K_NO_WAIT) == 0) {
			dropped_records++;
		} else {
			break;
		}
	}
	return 0;
}

int offline_log_sd_sync(void)
{
	struct offline_log_record rec;
	int rc = 0;

	if (!offline_log_sd_is_enabled()) {
		return -ENODEV;
	}

	/* If a remote download currently holds the writer file closed, wait
	 * for the transfer to finish (MCUmgr also auto-closes idle transfers)
	 * so the queued records can be flushed to SD. */
	for (;;) {
		bool hold;

		if (!offline_log_sd_is_enabled()) {
			return -ENODEV;
		}
		k_mutex_lock(&sd_lock, K_FOREVER);
		hold = sd_hold_for_download;
		k_mutex_unlock(&sd_lock);
		if (!hold) {
			break;
		}
		k_sleep(K_MSEC(50));
	}

	k_mutex_lock(&sd_lock, K_FOREVER);
	if (!card_mounted) {
		k_mutex_unlock(&sd_lock);
		return -ENODEV;
	}
	while (k_msgq_get(&ol_msgq, &rec, K_NO_WAIT) == 0) {
		rc = write_record_locked(&rec);
		if (rc != 0) {
			break;
		}
	}
	if (file_open) {
		(void)fs_sync(&log_file);
	}
	k_mutex_unlock(&sd_lock);
	return rc;
}

const char *offline_log_sd_mount_point(void)
{
	return sd_mount->mnt_point;
}

void offline_log_sd_get_status(struct offline_log_status *out)
{
	uint64_t free_kb = 0U;
	uint64_t total_kb = 0U;

	if (out == NULL) {
		return;
	}
	k_mutex_lock(&sd_lock, K_FOREVER);
	out->disabled = !offline_log_sd_is_enabled();
	out->last_mount_error = last_mount_error;
	out->card_mounted = card_mounted;
	out->file_open = file_open;
	out->time_trusted = time_trusted;
	out->queue_used = k_msgq_num_used_get(&ol_msgq);
	out->queue_max = OL_QUEUE_SIZE;
	out->dropped = dropped_records;
	out->bytes_written = total_bytes_written;
	out->files_removed = dirs_removed;
	out->pending_files_removed = pending_files_removed;
	if (card_mounted && sd_space_kb(&free_kb, &total_kb) == 0) {
		out->free_kb = free_kb;
		out->total_kb = total_kb;
	} else {
		out->free_kb = 0U;
		out->total_kb = 0U;
	}
	(void)snprintf(out->current_path, sizeof(out->current_path), "%s",
		       current_path);
	k_mutex_unlock(&sd_lock);
}

int offline_log_sd_cleanup_now(void)
{
	int rc;

	k_mutex_lock(&sd_lock, K_FOREVER);
	if (!card_mounted) {
		k_mutex_unlock(&sd_lock);
		return -ENODEV;
	}
	rc = cleanup_locked();
	k_mutex_unlock(&sd_lock);
	return rc;
}

int offline_log_sd_delete_date_dir(const char *date_tag)
{
	char full[176];
	char text[192];
	int rc;

	if (!parse_date_dir(date_tag, &(int64_t){0})) {
		return -EINVAL;
	}
	(void)snprintf(full, sizeof(full), "%s%s/%s",
		       sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR,
		       date_tag);
	k_mutex_lock(&sd_lock, K_FOREVER);
	if (!card_mounted) {
		k_mutex_unlock(&sd_lock);
		return -ENODEV;
	}
	rc = delete_dir_recursive_locked(full);
	if (rc == 0) {
		dirs_removed++;
		(void)snprintf(text, sizeof(text),
			       "offline log cleanup removed date_dir=%s "
			       "reason=manual",
			       date_tag);
		(void)offline_log_sd_enqueue(OFFLINE_LOG_CAT_SYSTEM,
					     OFFLINE_LOG_LEVEL_INF, text);
	} else {
		(void)snprintf(text, sizeof(text),
			       "offline log cleanup manual delete failed "
			       "date_dir=%s rc=%d",
			       date_tag, rc);
		(void)offline_log_sd_enqueue(OFFLINE_LOG_CAT_SYSTEM,
					     OFFLINE_LOG_LEVEL_ERR, text);
	}
	k_mutex_unlock(&sd_lock);
	return rc;
}

int offline_log_sd_delete_file(const char *path)
{
	char prefix[96];
	char full[192];
	char text[256];
	struct fs_dirent ent;
	size_t prefix_len;
	int rc;

	if (path == NULL || path[0] == '\0') {
		return -EINVAL;
	}

	rc = snprintf(prefix, sizeof(prefix), "%s%s/",
		      sd_mount->mnt_point, CONFIG_OFFLINE_LOG_BASE_DIR);
	if (rc <= 0 || (size_t)rc >= sizeof(prefix)) {
		return -ENAMETOOLONG;
	}
	prefix_len = (size_t)rc;

	/* Accept the full list path, or a path relative to the olog root. */
	if (strncmp(path, prefix, prefix_len) == 0) {
		(void)snprintf(full, sizeof(full), "%s", path);
	} else {
		const char *rel = path;

		while (*rel == '/') {
			rel++;
		}
		(void)snprintf(full, sizeof(full), "%s%s", prefix, rel);
	}

	/* Path whitelist: must stay inside /olog and point at one file. */
	if (strncmp(full, prefix, prefix_len) != 0 ||
	    strstr(full, "/../") != NULL ||
	    full[prefix_len] == '\0') {
		return -EACCES;
	}

	k_mutex_lock(&sd_lock, K_FOREVER);
	if (!card_mounted) {
		k_mutex_unlock(&sd_lock);
		return -ENODEV;
	}

	/* Never remove the file currently being appended... */
	if (file_open && strcmp(current_path, full) == 0) {
		k_mutex_unlock(&sd_lock);
		return -EBUSY;
	}
	/* ...or a file that is closed only because a download is reading it. */
	if (sd_hold_for_download && hold_reopen_valid &&
	    strcmp(hold_reopen_path, full) == 0) {
		k_mutex_unlock(&sd_lock);
		return -EBUSY;
	}

	rc = fs_stat(full, &ent);
	if (rc != 0) {
		k_mutex_unlock(&sd_lock);
		return rc;
	}
	if (ent.type == FS_DIR_ENTRY_DIR) {
		k_mutex_unlock(&sd_lock);
		return -EISDIR;
	}

	rc = fs_unlink(full);
	if (rc == 0) {
		(void)snprintf(text, sizeof(text),
			       "offline log file removed path=%s reason=manual",
			       full);
		(void)offline_log_sd_enqueue(OFFLINE_LOG_CAT_SYSTEM,
					     OFFLINE_LOG_LEVEL_INF, text);
	} else {
		(void)snprintf(text, sizeof(text),
			       "offline log file remove failed path=%s rc=%d",
			       full, rc);
		(void)offline_log_sd_enqueue(OFFLINE_LOG_CAT_SYSTEM,
					     OFFLINE_LOG_LEVEL_ERR, text);
	}
	k_mutex_unlock(&sd_lock);
	return rc;
}

/*
 * FatFs timestamp callback (required by CONFIG_FS_FATFS_HAS_RTC=y).
 * Uses trusted time when available, otherwise a fixed 2026-01-01 date so
 * newly created files/directories still get valid FAT timestamps.
 */
#if defined(CONFIG_FS_FATFS_HAS_RTC)
uint32_t get_fattime(void)
{
#if defined(CONFIG_TIME_MANAGER_SERVICE)
	struct tm tm_local;
	int64_t utc_ms;

	if (time_manager_has_trusted_time() &&
	    time_manager_get_utc_ms(&utc_ms) == 0) {
		utc_ms_to_local_tm(utc_ms, &tm_local);
		return ((uint32_t)(tm_local.tm_year + 1900 - 1980) << 25) |
		       ((uint32_t)(tm_local.tm_mon + 1) << 21) |
		       ((uint32_t)tm_local.tm_mday << 16) |
		       ((uint32_t)tm_local.tm_hour << 11) |
		       ((uint32_t)tm_local.tm_min << 5) |
		       ((uint32_t)(tm_local.tm_sec / 2));
	}
#endif
	return ((uint32_t)(2026 - 1980) << 25) |
	       (1U << 21) | (1U << 16);
}
#endif
