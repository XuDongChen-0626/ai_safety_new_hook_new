/*
 * offline_log_shell.c - "olog" shell commands for field diagnostics.
 */

#include "offline_log_internal.h"
#include "offline_log_service.h"

#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>

#include <stdio.h>
#include <string.h>

static int cmd_status(const struct shell *shell, size_t argc, char **argv)
{
	struct offline_log_status st;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	offline_log_get_status(&st);
	shell_print(shell,
		    "disabled=%d card_mounted=%d file_open=%d time_trusted=%d",
		    (int)st.disabled, (int)st.card_mounted, (int)st.file_open,
		    (int)st.time_trusted);
	shell_print(shell, "last_mount_error=%d", st.last_mount_error);
	shell_print(shell,
		    "queue=%u/%u dropped=%u bytes=%llu files_removed=%u "
		    "pending_files_removed=%u",
		    st.queue_used, st.queue_max, st.dropped,
		    (unsigned long long)st.bytes_written, st.files_removed,
		    st.pending_files_removed);
	shell_print(shell, "sd_space_free_kb=%llu total_kb=%llu",
		    (unsigned long long)st.free_kb,
		    (unsigned long long)st.total_kb);
	shell_print(shell, "current=%s",
		    st.current_path[0] != '\0' ? st.current_path : "(none)");
	return 0;
}

static int cmd_sync(const struct shell *shell, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = offline_log_sync();
	if (rc == 0) {
		shell_print(shell, "offline log synced");
	} else {
		shell_error(shell, "sync failed: %d", rc);
	}
	return rc;
}

static void list_dir(const struct shell *shell, const char *path, int depth)
{
	struct fs_dir_t dir;

	if (depth > 3) {
		return;
	}

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, path) != 0) {
		return;
	}
	for (;;) {
		struct fs_dirent ent;
		char full[200];

		if (fs_readdir(&dir, &ent) != 0 || ent.name[0] == '\0') {
			break;
		}
		if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) {
			continue;
		}
		(void)snprintf(full, sizeof(full), "%s/%s", path, ent.name);
		if (ent.type == FS_DIR_ENTRY_DIR) {
			shell_print(shell, "DIR  %s", full);
			list_dir(shell, full, depth + 1);
		} else {
			shell_print(shell, "FILE %s (%u B)", full,
				    (unsigned int)ent.size);
		}
	}
	(void)fs_closedir(&dir);
}

static int cmd_list(const struct shell *shell, size_t argc, char **argv)
{
	struct offline_log_status st;
	char root[160];
	int n;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	offline_log_get_status(&st);
	if (!st.card_mounted) {
		shell_error(shell, "SD card is not mounted");
		return -ENODEV;
	}

	n = snprintf(root, sizeof(root), "%s%s", offline_log_sd_mount_point(),
		     CONFIG_OFFLINE_LOG_BASE_DIR);
	if (n <= 0 || (size_t)n >= sizeof(root)) {
		return -ENAMETOOLONG;
	}
	list_dir(shell, root, 0);
	return 0;
}

static int cmd_cleanup(const struct shell *shell, size_t argc, char **argv)
{
	int rc;

	if (argc == 2) {
		rc = offline_log_sd_delete_date_dir(argv[1]);
	} else {
		rc = offline_log_sd_cleanup_now();
	}
	if (rc == 0) {
		shell_print(shell, "cleanup ok");
	} else {
		shell_error(shell, "cleanup failed: %d", rc);
	}
	return rc;
}

static int cmd_rm(const struct shell *shell, size_t argc, char **argv)
{
	int rc;

	if (argc < 2) {
		shell_error(shell, "usage: olog rm <file>");
		return -EINVAL;
	}

	rc = offline_log_sd_delete_file(argv[1]);
	if (rc == 0) {
		shell_print(shell, "deleted: %s", argv[1]);
	} else {
		shell_error(shell, "delete failed: %s rc=%d", argv[1], rc);
	}
	return rc;
}

static int cmd_demo(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!offline_log_sd_is_enabled()) {
		shell_error(shell, "offline log backend is disabled");
		return -ENODEV;
	}

	offline_log_event(OFFLINE_LOG_CAT_SYSTEM, OFFLINE_LOG_LEVEL_INF,
			  "demo: system event");
	offline_log_event(OFFLINE_LOG_CAT_POWER, OFFLINE_LOG_LEVEL_INF,
			  "demo: power event");
	offline_log_event(OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_WRN,
			  "demo: network event");
	offline_log_event(OFFLINE_LOG_CAT_BMS, OFFLINE_LOG_LEVEL_WRN,
			  "demo: bms offline (test)");
	offline_log_event(OFFLINE_LOG_CAT_ALARM, OFFLINE_LOG_LEVEL_WRN,
			  "demo: alarm module offline (test)");
	offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
			  "demo: ota event (test)");
	offline_log_event(OFFLINE_LOG_CAT_FAULT, OFFLINE_LOG_LEVEL_ERR,
			  "demo: fault event (test)");
	offline_log_event(OFFLINE_LOG_CAT_PARAM, OFFLINE_LOG_LEVEL_INF,
			  "demo: param event (test)");
	(void)offline_log_sync();
	shell_print(shell, "demo events queued and synced");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	olog_cmds,
	SHELL_CMD(status, NULL, "Offline log status.", cmd_status),
	SHELL_CMD(sync, NULL, "Flush queued records to the SD card.", cmd_sync),
	SHELL_CMD(list, NULL, "List offline log files.", cmd_list),
	SHELL_CMD_ARG(cleanup, NULL,
		      "Run cleanup: `olog cleanup` or `olog cleanup YYYY-MM-DD`.",
		      cmd_cleanup, 1, 1),
	SHELL_CMD_ARG(rm, NULL,
		      "Delete one log file: `olog rm <file>` "
		      "(full path or path under /olog).",
		      cmd_rm, 2, 0),
	SHELL_CMD(demo, NULL, "Queue sample events for every category.",
		  cmd_demo),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(olog, &olog_cmds, "SD offline log commands.", NULL);
