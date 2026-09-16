/*
 * hook_shell.c - product shell commands.
 *
 * These commands are the Zephyr-native equivalent of the legacy
 * "craner#AT..." command surface (which is still accepted on the 4G/cloud
 * channel, see app/hook_4g/hook_at.c).
 */
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/timeutil.h>

#include "board.h"
#include "hook_at.h"
#include "hook_4g.h"
#include "hook_ota.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "hook_relay.h"
#include "ota_manager_service.h"
#include "system_def.h"
#include "uart_manage_port.h"

#define HOOK_SHELL_RTC DEVICE_DT_GET(DT_NODELABEL(rtc))

static int cmd_hook_status(const struct shell *sh, size_t argc, char **argv)
{
	int64_t unix_s;
	struct rtc_time tm;
	char time_text[48];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	(void)snprintf(time_text, sizeof(time_text), "unavailable");

	if ((rtc_get_time(HOOK_SHELL_RTC, &tm) == 0) &&
	    (timeutil_timegm64(&(struct tm){
		     .tm_sec = tm.tm_sec, .tm_min = tm.tm_min,
		     .tm_hour = tm.tm_hour, .tm_mday = tm.tm_mday,
		     .tm_mon = tm.tm_mon, .tm_year = tm.tm_year,
	     }) > 0)) {
		struct tm utc = {
			.tm_sec = tm.tm_sec, .tm_min = tm.tm_min,
			.tm_hour = tm.tm_hour, .tm_mday = tm.tm_mday,
			.tm_mon = tm.tm_mon, .tm_year = tm.tm_year,
		};

		unix_s = timeutil_timegm64(&utc);
		if (unix_s > 0) {
			(void)snprintf(time_text, sizeof(time_text),
				       "%04u-%02u-%02u %02u:%02u:%02u UTC (RTC)",
				       (unsigned int)(tm.tm_year + 1900),
				       (unsigned int)(tm.tm_mon + 1),
				       (unsigned int)tm.tm_mday,
				       (unsigned int)tm.tm_hour,
				       (unsigned int)tm.tm_min,
				       (unsigned int)tm.tm_sec);
		}
	}

	shell_print(sh, "work mode      : %u (1 normal, 2 standby, 3 error)",
		    hook_reg_get(STATUS_WORK_MODE));
	shell_print(sh, "error code     : 0x%02X",
		    hook_reg_get(REG_ERROR_CODE));
	shell_print(sh, "relay          : %s",
		    hook_relay_is_on() ? "on" : "off");
	shell_print(sh, "soft standby   : %u (register %u)", is_soft_standby,
		    SOFT_STANDBY_ENABLE);
	shell_print(sh, "heartbeat      : register %u = %u, enable = %u",
		    STATUS_HEART_BEAT, hook_reg_get(STATUS_HEART_BEAT),
		    hook_reg_get(HEARTBEAT_ENABLE));
	shell_print(sh, "battery        : %u (low threshold %u)",
		    hook_reg_get(STATUS_BMS_BATTERY), LOW_BATTERY_THRESHOLD);
	shell_print(sh, "alarm volume   : %u", hook_reg_get(CMD_VOLUME));
	shell_print(sh, "ota            : %s (%u/%u B)",
		    hook_ota_state_name(hook_ota_get_state()),
		    hook_ota_bytes_received(), hook_ota_image_size());
	shell_print(sh, "power schedule : on %02u:%02u off %02u:%02u (Beijing)",
		    hook_reg_get(STATUS_POWER_ON_TIME) >> 8,
		    hook_reg_get(STATUS_POWER_ON_TIME) & 0xFFU,
		    hook_reg_get(STATUS_POWER_OFF_TIME) >> 8,
		    hook_reg_get(STATUS_POWER_OFF_TIME) & 0xFFU);
	shell_print(sh, "rtc            : %s", time_text);

	return 0;
}

static int cmd_hook_reg(const struct shell *sh, size_t argc, char **argv)
{
	long index;
	char *end;

	if ((argc < 2U) || (argc > 3U)) {
		shell_error(sh, "usage: hook reg <index> [value]");
		return -EINVAL;
	}

	index = strtol(argv[1], &end, 0);
	if ((*end != '\0') || (index < 0) || (index >= (long)HOOK_REGISTER_COUNT)) {
		shell_error(sh, "register index out of range (0..%u)",
			    HOOK_REGISTER_COUNT - 1U);
		return -EINVAL;
	}

	if (argc == 2U) {
		shell_print(sh, "reg[%ld] = %u (0x%04X)", index,
			    hook_reg_get((uint16_t)index),
			    hook_reg_get((uint16_t)index));
		return 0;
	}

	{
		unsigned long value = strtoul(argv[2], &end, 0);

		if (*end != '\0') {
			shell_error(sh, "invalid value '%s'", argv[2]);
			return -EINVAL;
		}

		hook_reg_set((uint16_t)index, (uint16_t)value);
		shell_print(sh, "reg[%ld] <= %u (0x%04X)", index,
			    hook_reg_get((uint16_t)index),
			    hook_reg_get((uint16_t)index));
	}

	return 0;
}

#define HOOK_SHELL_AT_LINE_MAX 128U
#define HOOK_USR_PREFIX "usr.cn#"

/* Shortcuts for the module queries used most often during bring-up. */
struct hook_usr_shortcut {
	const char *name;
	const char *at_cmd;
};

static const struct hook_usr_shortcut hook_usr_shortcuts[] = {
	{"wk_mod", "AT+WKMOD"},           /* 工作模式 */
	{"mqtt_pub_tp", "AT+MQTTPUBTP"},  /* MQTT 发布话题 */
	{"mqtt_sub_tp", "AT+MQTTSUBTP"},  /* MQTT 订阅话题 */
	{"mqtt_sta", "AT+MQTTSTA"},       /* MQTT 连接状态 */
	{"mqtt_svr", "AT+MQTTSVR"},       /* MQTT 服务器 */
	{"mqtt_user", "AT+MQTTUSER"},     /* MQTT 用户名 */
	{"mqtt_psw", "AT+MQTTPSW"},       /* MQTT 密码 */
	{"mqtt_cid", "AT+MQTTCID"},       /* MQTT Client ID */
};

/*
 * Join the shell arguments into "<prefix><args>" (no line terminator; the
 * AT/USR handlers add what the target needs). When @p prefix is NULL the
 * first argument is used as-is.
 */
static size_t hook_shell_build_line(char *line, size_t line_size,
				    const char *prefix, size_t argc,
				    char **argv)
{
	size_t used = 0U;

	if (prefix != NULL) {
		int written = snprintf(line, line_size, "%s", prefix);

		if (written <= 0) {
			return 0U;
		}
		used = (size_t)written;
	}

	for (size_t i = 1U; (i < argc) && (used < (line_size - 2U)); i++) {
		int written = snprintf(&line[used], line_size - used, "%s%s",
				       ((i == 1U) && (prefix != NULL)) ? "" : " ",
				       argv[i]);

		if (written <= 0) {
			break;
		}
		used += (size_t)written;
	}

	if (used >= (line_size - 2U)) {
		return 0U;
	}

	line[used] = '\0';

	return used;
}

/*
 * Forward a command line to the usr.cn 4G module, e.g.
 *   hook usr AT+Z            -> sends "usr.cn#AT+Z\r\n"
 *   hook usr usr.cn#AT+Z     -> sends it unchanged
 * The module answer is printed on this console by the "4g" receive path.
 */
static int cmd_hook_usr(const struct shell *sh, size_t argc, char **argv)
{
	char line[HOOK_SHELL_AT_LINE_MAX];
	const char *prefix = NULL;
	const char *shortcut_cmd = NULL;
	size_t used;

	/*
	 * "hook usr hex on|off"    : hex echo until turned off (counters reset
	 *                            on enable, so the dump of one answer is
	 *                            unambiguous)
	 * "hook usr hex <AT command>": enable for 10 s and send the command
	 */
	if ((argc >= 2U) && (strcmp(argv[1], "hex") == 0)) {
		if ((argc >= 3U) && (strcmp(argv[2], "on") == 0)) {
			hook_4g_enable_hex_echo(true);
			shell_print(sh, "hex echo on (use \"hook usr hex off\" to stop)");
			return 0;
		}
		if ((argc >= 3U) && (strcmp(argv[2], "off") == 0)) {
			hook_4g_enable_hex_echo(false);
			shell_print(sh, "hex echo off");
			return 0;
		}

		hook_4g_enable_hex_echo(true);
		argc--;
		argv++;
	}

	if (argc < 2U) {
		shell_error(sh, "usage: hook usr [hex on|off] <AT command|shortcut>");
		shell_print(sh, "e.g. hook usr AT+Z | hook usr mqtt_sta | hook usr wk_mod");
		return -EINVAL;
	}

	/* Shortcut name -> AT command. */
	for (size_t i = 0U; i < ARRAY_SIZE(hook_usr_shortcuts); i++) {
		if (strcmp(argv[1], hook_usr_shortcuts[i].name) == 0) {
			if (argc > 2U) {
				shell_error(sh, "shortcut '%s' takes no argument",
					    argv[1]);
				return -EINVAL;
			}
			shortcut_cmd = hook_usr_shortcuts[i].at_cmd;
			break;
		}
	}

	if (shortcut_cmd != NULL) {
		int written = snprintf(line, sizeof(line), "%s%s", HOOK_USR_PREFIX,
				       shortcut_cmd);

		used = (written > 0) ? (size_t)written : 0U;
	} else {
		if (strncmp(argv[1], HOOK_USR_PREFIX, strlen(HOOK_USR_PREFIX)) != 0) {
			prefix = HOOK_USR_PREFIX;
		}

		used = hook_shell_build_line(line, sizeof(line), prefix, argc, argv);
	}

	if (used == 0U) {
		shell_error(sh, "command line too long");
		return -EINVAL;
	}

	/* Keep the whole command in one UART frame: usr.cn needs CRLF. */
	if (used > (sizeof(line) - 3U)) {
		shell_error(sh, "command line too long");
		return -EINVAL;
	}
	line[used++] = '\r';
	line[used++] = '\n';
	line[used] = '\0';

	if (hook_at_usr_forward((const uint8_t *)line, (uint16_t)used) != HOOK_AT_OK) {
		shell_error(sh, "could not send the usr.cn command");
		return -EIO;
	}

	return 0;
}

static int cmd_hook_craner(const struct shell *sh, size_t argc, char **argv)
{
	char line[HOOK_SHELL_AT_LINE_MAX];
	size_t used;
	int32_t ret;

	if (argc < 2U) {
		shell_error(sh, "usage: hook craner <command>  (e.g. hook craner AT+OTAINFO)");
		return -EINVAL;
	}

	used = hook_shell_build_line(line, sizeof(line), "craner#", argc, argv);
	if (used == 0U) {
		shell_error(sh, "command line too long");
		return -EINVAL;
	}

	ret = hook_at_craner_handler((const uint8_t *)line, (uint16_t)used,
				     hook_console_send);
	if (ret == HOOK_AT_UNKNOWN_CMD) {
		shell_error(sh, "unknown AT command");
		return -ENOTSUP;
	}
	if (ret != HOOK_AT_OK) {
		shell_error(sh, "AT command failed (%d)", ret);
		return -EIO;
	}

	/* Responses are printed to the console by the reply callbacks. */
	return 0;
}

static int cmd_hook_ota_info(const struct shell *sh, size_t argc, char **argv)
{
	struct ota_manager_image_info info;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (uint8_t slot = 0U; slot < 2U; slot++) {
		int ret = ota_manager_get_image_info(slot, &info);

		if ((ret != 0) || !info.valid) {
			shell_print(sh, "slot%u: empty", slot);
			continue;
		}

		shell_print(sh, "slot%u: version=%s size=%u flags=0x%02X hash=%s",
			    slot, info.version, info.image_size,
			    info.state_flags, info.hash_hex);
	}

	return 0;
}

static int cmd_hook_ota_confirm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (ota_manager_confirm_current_image() != 0) {
		shell_error(sh, "image confirm failed");
		return -EIO;
	}

	shell_print(sh, "running image confirmed");

	return 0;
}

static int cmd_hook_ota_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (hook_ota_request() != 0) {
		shell_error(sh, "could not start the OTA receiver");
		return -EIO;
	}

	shell_print(sh, "OTA receiver armed, waiting for a Ymodem transfer");

	return 0;
}

static int cmd_hook_ota_cancel(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	hook_ota_cancel();
	shell_print(sh, "OTA session cancelled");

	return 0;
}

static int cmd_hook_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "rebooting...");
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	hook_ota_cmds,
	SHELL_CMD(start, NULL, "Arm the Ymodem OTA receiver (writes slot1).",
		  cmd_hook_ota_start),
	SHELL_CMD(cancel, NULL, "Abort the running OTA session.",
		  cmd_hook_ota_cancel),
	SHELL_CMD(info, NULL, "Show MCUboot image information.", cmd_hook_ota_info),
	SHELL_CMD(confirm, NULL, "Confirm the running image.", cmd_hook_ota_confirm),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	hook_cmds,
	SHELL_CMD(status, NULL, "Show hook controller status.", cmd_hook_status),
	SHELL_CMD_ARG(reg, NULL, "Read/write a Modbus register: reg <index> [value]",
		      cmd_hook_reg, 2, 1),
	SHELL_CMD_ARG(craner, NULL, "Run a craner#AT command: craner <command>",
		      cmd_hook_craner, 2, 8),
	SHELL_CMD_ARG(usr, NULL,
		      "4G module command: usr <AT command|shortcut>  "
		      "(shortcuts: wk_mod, mqtt_sta, mqtt_pub_tp, mqtt_sub_tp, "
		      "mqtt_svr, mqtt_user, mqtt_psw, mqtt_cid; add \"hex\" to "
		      "dump the frames)",
		      cmd_hook_usr, 2, 8),
	SHELL_CMD(ota, &hook_ota_cmds, "OTA image commands.", NULL),
	SHELL_CMD(reset, NULL, "Cold reboot the controller.", cmd_hook_reset),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(hook, &hook_cmds, "Craner AI safety hook commands.", NULL);
