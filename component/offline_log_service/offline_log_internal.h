/*
 * offline_log_internal.h - private interface between the event API and the
 * SD writer, plus hooks used by shell/mcumgr glue.
 */

#ifndef OFFLINE_LOG_INTERNAL_H_
#define OFFLINE_LOG_INTERNAL_H_

#include "offline_log_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the mount/writer thread and mark the service initialized. */
int offline_log_sd_start(void);

/** False after the SD backend exhausted its bounded mount attempts. */
bool offline_log_sd_is_enabled(void);

/** Queue one already-formatted message body (writer thread owns the SD). */
int offline_log_sd_enqueue(enum offline_log_category cat,
			   enum offline_log_level level,
			   const char *text);

/** Flush queued records synchronously. */
int offline_log_sd_sync(void);

/** Returns the FATFS mount point string (e.g. "/SD:"). */
const char *offline_log_sd_mount_point(void);

/** Run the retention/size cleanup immediately. */
int offline_log_sd_cleanup_now(void);

/** Re-export status fields used by shell. */
void offline_log_sd_get_status(struct offline_log_status *out);

/** Recursively delete one date directory (shell cleanup). */
int offline_log_sd_delete_date_dir(const char *date_tag);

/** Delete one offline log file (path may be full or relative to /olog). */
int offline_log_sd_delete_file(const char *path);

/** Close the currently open log file if it matches path (remote download). */
int offline_log_sd_hold_open_file_for_read(const char *path);

/** Resume the writer after the remote file transfer finished. */
void offline_log_sd_release_after_read(void);

#ifdef __cplusplus
}
#endif

#endif /* OFFLINE_LOG_INTERNAL_H_ */
