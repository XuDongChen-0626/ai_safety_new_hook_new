/*
 * offline_log_mgmt.c - MCUmgr glue.
 *
 *  - FS group: allow download/status/checksum of the offline log root only;
 *    uploads and any other path are denied. File transfers are recorded as
 *    offline log events.
 *  - IMG group: firmware upload / pending / confirm / stop lifecycle is
 *    recorded as OTA events.
 */

#include "offline_log_internal.h"
#include "offline_log_service.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>

#include <string.h>

#if defined(CONFIG_OTA_MANAGER_SERVICE)
#include "ota_manager_service.h"
#endif

#if defined(CONFIG_MCUMGR_GRP_FS)
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt_callbacks.h>
#endif
#if defined(CONFIG_MCUMGR_GRP_IMG)
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#endif

#if defined(CONFIG_MCUMGR_GRP_FS) || defined(CONFIG_MCUMGR_GRP_IMG)

#if defined(CONFIG_MCUMGR_GRP_FS)
static const char *access_type_name(int access)
{
	switch (access) {
	case FS_MGMT_FILE_ACCESS_READ:
		return "read";
	case FS_MGMT_FILE_ACCESS_WRITE:
		return "write";
	case FS_MGMT_FILE_ACCESS_STATUS:
		return "status";
	case FS_MGMT_FILE_ACCESS_HASH_CHECKSUM:
		return "checksum";
	default:
		return "?";
	}
}

/* A single fs download is split into many MCUmgr chunks and the FS access
 * hook fires for every chunk. Track the active transfer so the offline log
 * records one "start" and one "done" per file instead of one line per chunk.
 */
static char fs_active_path[160];
static int fs_active_access;
static bool fs_active_valid;

/* fs_mgmt may reopen the same file right after a transfer when a UDP
 * response is lost and the client retransmits: the first session completes,
 * then a second (usually zero-byte) session starts within milliseconds.
 * Suppress logging that quick restart so one download is recorded once. */
#define FS_DONE_QUIET_WINDOW_MS 1500U
static char fs_suppress_path[160];
static uint32_t fs_suppress_until_ms;
static bool fs_silent_session;

static bool path_is_allowed(const char *path, int access)
{
	char prefix[96];
	int n;

	if (path == NULL || path[0] == '\0') {
		return false;
	}

	n = snprintf(prefix, sizeof(prefix), "%s%s/",
		     offline_log_sd_mount_point(),
		     CONFIG_OFFLINE_LOG_BASE_DIR);
	if (n <= 0 || (size_t)n >= sizeof(prefix)) {
		return false;
	}

	/* Remote download/status/checksum is allowed under /olog only. */
	if (access != FS_MGMT_FILE_ACCESS_WRITE) {
		return strncmp(path, prefix, strlen(prefix)) == 0;
	}

	/* Remote upload is denied (no upload directory is provisioned). */
	return false;
}

static enum mgmt_cb_return fs_access_cb(uint32_t event,
					enum mgmt_cb_return prev_status,
					int32_t *rc, uint16_t *group,
					bool *abort_more, void *data,
					size_t data_size)
{
	struct fs_mgmt_file_access *acc = data;

	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data_size);

	if (event == MGMT_EVT_OP_FS_MGMT_FILE_ACCESS) {
		if (acc == NULL || !path_is_allowed(acc->filename,
						    acc->access)) {
			if (acc != NULL) {
				(void)offline_log_event(
					OFFLINE_LOG_CAT_SYSTEM,
					OFFLINE_LOG_LEVEL_WRN,
					"mcumgr fs denied: access=%s path=%s",
					access_type_name(acc->access),
					acc->filename != NULL ?
					acc->filename : "?");
			}
			*rc = MGMT_ERR_EINVAL;
			return MGMT_CB_ERROR_RC;
		}

		if (!fs_active_valid ||
		    fs_active_access != acc->access ||
		    strncmp(fs_active_path, acc->filename,
			    sizeof(fs_active_path) - 1U) != 0) {
			const bool quickRestart =
				acc->access == FS_MGMT_FILE_ACCESS_READ &&
				fs_suppress_until_ms != 0U &&
				k_uptime_get_32() < fs_suppress_until_ms &&
				strncmp(fs_suppress_path, acc->filename,
					sizeof(fs_suppress_path) - 1U) == 0;

			/* Downloading the currently open log file: close it
			 * first (writer pauses, events stay in the RAM queue)
			 * so the remote side gets a consistent snapshot. */
			/* Quick-restart (retransmission) sessions skip this so
			 * the writer file is not closed/reopened repeatedly. */
			if (acc->access == FS_MGMT_FILE_ACCESS_READ &&
			    !quickRestart) {
				(void)offline_log_sd_hold_open_file_for_read(
					acc->filename);
			}

			if (quickRestart) {
				fs_silent_session = true;
			} else {
				fs_silent_session = false;
				(void)offline_log_event(
					OFFLINE_LOG_CAT_SYSTEM,
					OFFLINE_LOG_LEVEL_INF,
					"mcumgr fs start: access=%s path=%s",
					access_type_name(acc->access),
					acc->filename != NULL ?
					acc->filename : "?");
			}
			(void)snprintf(fs_active_path,
				       sizeof(fs_active_path), "%s",
				       acc->filename != NULL ?
				       acc->filename : "");
			fs_active_access = acc->access;
			fs_active_valid = true;
		}
		return MGMT_CB_OK;
	}

	if (event == MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE && acc != NULL) {
		if (fs_active_valid &&
		    fs_active_access == acc->access &&
		    strncmp(fs_active_path, acc->filename,
			    sizeof(fs_active_path) - 1U) == 0) {
			const bool wasSilent = fs_silent_session;

			if (!wasSilent) {
				(void)offline_log_event(
					OFFLINE_LOG_CAT_SYSTEM,
					OFFLINE_LOG_LEVEL_INF,
					"mcumgr fs done: access=%s path=%s",
					access_type_name(acc->access),
					acc->filename != NULL ?
					acc->filename : "?");
			}
			fs_silent_session = false;
			if (!wasSilent) {
				(void)offline_log_sd_release_after_read();
			}
			fs_active_valid = false;

			(void)snprintf(fs_suppress_path,
				       sizeof(fs_suppress_path), "%s",
				       acc->filename != NULL ?
				       acc->filename : "");
			fs_suppress_until_ms =
				k_uptime_get_32() + FS_DONE_QUIET_WINDOW_MS;
		}
	}
	return MGMT_CB_OK;
}
#endif /* CONFIG_MCUMGR_GRP_FS */

#if defined(CONFIG_MCUMGR_GRP_IMG)
static enum mgmt_cb_return img_dfu_cb(uint32_t event,
				      enum mgmt_cb_return prev_status,
				      int32_t *rc, uint16_t *group,
				      bool *abort_more, void *data,
				      size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	switch (event) {
	case MGMT_EVT_OP_IMG_MGMT_DFU_STARTED:
		offline_log_event(OFFLINE_LOG_CAT_OTA,
				  OFFLINE_LOG_LEVEL_INF,
				  "OTA upload started");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
		offline_log_event(OFFLINE_LOG_CAT_OTA,
				  OFFLINE_LOG_LEVEL_INF,
				  "OTA upload complete, image pending test");
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_CONFIRMED:
		offline_log_event(OFFLINE_LOG_CAT_OTA,
				  OFFLINE_LOG_LEVEL_INF,
				  "OTA image confirmed via mcumgr");
#if defined(CONFIG_OTA_MANAGER_SERVICE)
		/* MCUmgr confirms the image through its own path, which does not
		 * call the OTA manager API. Cancel its rollback timer here as
		 * well, otherwise the "image not confirmed" warnings continue
		 * (and would eventually reboot) even though the image is
		 * confirmed. */
		(void)ota_manager_confirm_current_image();
#endif
		break;
	case MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED:
		offline_log_event(OFFLINE_LOG_CAT_OTA,
				  OFFLINE_LOG_LEVEL_WRN,
				  "OTA upload stopped");
		break;
	default:
		break;
	}
	return MGMT_CB_OK;
}
#endif /* CONFIG_MCUMGR_GRP_IMG */

#if defined(CONFIG_MCUMGR_GRP_FS)
static struct mgmt_callback cb_fs;
#endif
#if defined(CONFIG_MCUMGR_GRP_IMG)
static struct mgmt_callback cb_img;
#endif

static int offline_log_mgmt_init(void)
{
#if defined(CONFIG_MCUMGR_GRP_FS)
	cb_fs.event_id = MGMT_EVT_OP_FS_MGMT_FILE_ACCESS |
			 MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE;
	cb_fs.callback = fs_access_cb;
	mgmt_callback_register(&cb_fs);
#endif
#if defined(CONFIG_MCUMGR_GRP_IMG)
	cb_img.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_STARTED |
			  MGMT_EVT_OP_IMG_MGMT_DFU_PENDING |
			  MGMT_EVT_OP_IMG_MGMT_DFU_CONFIRMED |
			  MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED;
	cb_img.callback = img_dfu_cb;
	mgmt_callback_register(&cb_img);
#endif
	return 0;
}

SYS_INIT(offline_log_mgmt_init, APPLICATION, 96);

#endif /* CONFIG_MCUMGR_GRP_FS || CONFIG_MCUMGR_GRP_IMG */
