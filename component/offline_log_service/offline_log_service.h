/*
 * offline_log_service.h - curated "black-box" event log API.
 *
 * Product code records important, non-repeating state changes through
 * offline_log_event(). The service formats one line per event and writes it
 * to the SD card:
 *
 *   [2026-09-05T16:20:33.123+08:00] <wrn> [bms] BMS offline reason=modbus timeout
 *
 * The Zephyr LOG stream is intentionally NOT mirrored to the SD card; only
 * events that product code considers worth keeping are recorded.
 */

#ifndef OFFLINE_LOG_SERVICE_H_
#define OFFLINE_LOG_SERVICE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log levels stored in the file. */
enum offline_log_level {
	OFFLINE_LOG_LEVEL_ERR = 1,
	OFFLINE_LOG_LEVEL_WRN = 2,
	OFFLINE_LOG_LEVEL_INF = 3,
	OFFLINE_LOG_LEVEL_DBG = 4,
};

/*
 * Event categories. A category is written to every line so PC-side tools and
 * shell users can filter a daily file by module (BMS/MPPT/laser/network/...).
 */
enum offline_log_category {
	OFFLINE_LOG_CAT_SYSTEM = 0,  /* boot, reset, generic system state   */
	OFFLINE_LOG_CAT_POWER,       /* power rails, sleep, standby         */
	OFFLINE_LOG_CAT_NETWORK,     /* 4G link: heartbeat/quality, cloud   */
	OFFLINE_LOG_CAT_BMS,         /* battery management system           */
	OFFLINE_LOG_CAT_ALARM,       /* sound & light alarm module          */
	OFFLINE_LOG_CAT_OTA,         /* firmware update / confirm / revert  */
	OFFLINE_LOG_CAT_FAULT,       /* fatal record, reset reason, coredump */
	OFFLINE_LOG_CAT_PARAM,       /* parameter table dump (boot / save)  */
	OFFLINE_LOG_CAT_COUNT,
};

/** Human-readable category tag (e.g. "bms"), used in log lines. */
const char *offline_log_category_name(enum offline_log_category cat);

/** Human-readable level tag (e.g. "wrn"). */
const char *offline_log_level_name(enum offline_log_level level);

/**
 * Record one curated event. The call is non-blocking: the formatted message
 * is queued and written to the SD card by the background writer thread.
 * When the queue is full the oldest pending record is dropped.
 *
 * @param cat    event category
 * @param level  event level
 * @param fmt    printf-style message
 */
int offline_log_event(enum offline_log_category cat,
		      enum offline_log_level level,
		      const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/**
 * Record a state transition only when the boolean state actually changes.
 * This is the recommended helper for "record online/offline once, do not
 * repeat every poll cycle".
 *
 * @param cat       event category
 * @param level     level used for both messages
 * @param current   caller-owned current state; *current is updated to next
 * @param next      newly observed state
 * @param on_msg    message when state changes 0 -> 1
 * @param off_msg   message when state changes 1 -> 0
 */
void offline_log_transition(enum offline_log_category cat,
			    enum offline_log_level level,
			    bool *current, bool next,
			    const char *on_msg, const char *off_msg);

/** Synchronously drain queued records to the SD card (shell/test use). */
int offline_log_sync(void);

struct offline_log_status {
	bool disabled;          /* backend disabled after bounded mount failures */
	int last_mount_error;   /* most recent SD mount/remount error */
	bool card_mounted;      /* SD card is mounted */
	bool file_open;         /* a log file is open */
	bool time_trusted;      /* trusted UTC time is available */
	uint32_t queue_used;    /* queued records */
	uint32_t queue_max;     /* queue capacity */
	uint32_t dropped;       /* records dropped because the queue was full */
	uint64_t bytes_written; /* bytes written since boot */
	uint32_t files_removed; /* date directories removed by cleanup */
	uint32_t pending_files_removed; /* pending files removed by cap guard */
	uint64_t free_kb;       /* free SD space (0 when not mounted) */
	uint64_t total_kb;      /* total SD capacity (0 when not mounted) */
	char current_path[160]; /* current log file (empty when none) */
};

void offline_log_get_status(struct offline_log_status *out);

#ifdef __cplusplus
}
#endif

#endif /* OFFLINE_LOG_SERVICE_H_ */
