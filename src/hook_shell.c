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
#include "hook_4g_cfg.h"
#include "hook_ota.h"
#include "hook_params.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "hook_heartbeat.h"
#include "hook_rtc.h"
#include "ota_manager_service.h"
#include "system_def.h"
#include "time_manager_service.h"
#include "uart_manage_port.h"

#define HOOK_SHELL_RTC DEVICE_DT_GET(DT_NODELABEL(rtc))

static int cmd_hook_status(const struct shell *sh, size_t argc, char **argv)
{
	struct rtc_time tm;
	uint16_t cal_year;
	uint16_t cal_month_day;
	uint16_t cal_hhmm;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "work mode      : %u (1 normal, 2 standby, 3 error)",
		    hook_reg_input_get(STATUS_WORK_MODE));
	shell_print(sh, "device         : %s (device.no=\"%s\")",
		    hook_params_device_tag(), hook_params_device_no());
	shell_print(sh, "error code     : 0x%02X",
		    hook_reg_input_get(REG_ERROR_CODE));
	shell_print(sh, "alarm power    : %s",
		    hook_heartbeat_alarm_power_is_on() ? "on" : "off");
	shell_print(sh, "soft standby   : %u (register %u)", is_soft_standby,
		    SOFT_STANDBY_ENABLE);
	shell_print(sh, "heartbeat      : register %u = %u, enable = %u",
		    CMD_HOST_HEARTBEAT, hook_reg_get(CMD_HOST_HEARTBEAT),
		    hook_reg_get(HEARTBEAT_ENABLE));
	shell_print(sh, "battery        : %u (low threshold %u)",
		    hook_reg_input_get(STATUS_BMS_BATTERY), LOW_BATTERY_THRESHOLD);
	shell_print(sh, "alarm volume   : %u", hook_reg_get(CMD_VOLUME));
	shell_print(sh, "ota            : %s (%u/%u B)",
		    hook_ota_state_name(hook_ota_get_state()),
		    hook_ota_bytes_received(), hook_ota_image_size());
	shell_print(sh, "4g provisioning: %s (cid=%s)",
		    hook_4g_cfg_state_name(hook_4g_cfg_get_state()),
		    hook_4g_cfg_cid());
	shell_print(sh, "power schedule : on %02u:%02u off %02u:%02u (Beijing)",
		    hook_reg_get(POWER_ON_TIME) >> 8,
		    hook_reg_get(POWER_ON_TIME) & 0xFFU,
		    hook_reg_get(POWER_OFF_TIME) >> 8,
		    hook_reg_get(POWER_OFF_TIME) & 0xFFU);
	shell_print(sh, "standby enable : soft=%u hard=%u",
		    hook_reg_get(SOFT_STANDBY_ENABLE),
		    hook_reg_get(STM32_STANDBY_ENABLE));

	if (rtc_get_time(HOOK_SHELL_RTC, &tm) == 0) {
		shell_print(sh, "rtc (UTC)      : %04d-%02d-%02d %02d:%02d:%02d",
			    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			    tm.tm_hour, tm.tm_min, tm.tm_sec);
	} else {
		shell_print(sh, "rtc (UTC)      : unavailable");
	}

	cal_year = hook_reg_input_get(INPUT_RTC_YEAR);
	cal_month_day = hook_reg_input_get(INPUT_RTC_MONTH_DAY);
	cal_hhmm = hook_reg_input_get(INPUT_RTC_TIME_HHMM);

	shell_print(sh, "rtc (Beijing)  : %04u-%02u-%02u %02u:%02u (I%u/I%u/I%u)",
		    cal_year, (cal_month_day >> 8) & 0xFFU, cal_month_day & 0xFFU,
		    (cal_hhmm >> 8) & 0xFFU, cal_hhmm & 0xFFU,
		    INPUT_RTC_YEAR, INPUT_RTC_MONTH_DAY, INPUT_RTC_TIME_HHMM);
	shell_print(sh, "rtc trusted    : %s",
		    hook_rtc_trust_is_valid() ? "yes" : "no");
	shell_print(sh, "rtc calibration: waiting for H%u=%04u H%u=%04X H%u=%04X",
		    RTC_CAL_YEAR, hook_reg_get(RTC_CAL_YEAR),
		    RTC_CAL_MONTH_DAY, hook_reg_get(RTC_CAL_MONTH_DAY),
		    RTC_CAL_HHMM, hook_reg_get(RTC_CAL_HHMM));

	return 0;
}

/*
 * 4G link quality: statistics of the host heartbeat reception (the crane host
 * writes the heartbeat counter once per second, so the gaps between two
 * received heartbeats describe the downlink stability).
 */
static int cmd_hook_link(const struct shell *sh, size_t argc, char **argv)
{
	struct hook_link_stats st;

	if ((argc == 2U) && (strcmp(argv[1], "clear") == 0)) {
		hook_heartbeat_clear_link_stats();
		shell_print(sh, "link statistics cleared");
		return 0;
	}

	if (argc > 2U) {
		shell_error(sh, "usage: hook link [clear]");
		return -EINVAL;
	}

	hook_heartbeat_get_link_stats(&st);

	shell_print(sh, "device         : %s (device.no=\"%s\")",
		    hook_params_device_tag(), hook_params_device_no());
	shell_print(sh, "heartbeats     : %u received, %u timeout(s) (gap > %u ms)",
		    st.received, st.timeouts,
		    (unsigned int)CONFIG_HOOK_LINK_TIMEOUT_MS);
	shell_print(sh, "interval       : min %u / avg %u / max %u ms",
		    st.interval_min_ms, st.interval_avg_ms, st.interval_max_ms);
	shell_print(sh, "longest gap    : %u ms (counted as timeout)",
		    st.timeout_max_ms);

	if (st.last_timeout_unix_s > 0) {
		char iso[40];

		if (time_manager_format_local_iso8601_from_utc_ms(
			    st.last_timeout_unix_s * MSEC_PER_SEC, iso,
			    sizeof(iso)) == 0) {
			shell_print(sh, "last timeout   : %s (uptime %u ms)", iso,
				    st.last_timeout_ms);
		} else {
			shell_print(sh, "last timeout   : uptime %u ms",
				    st.last_timeout_ms);
		}
	} else {
		shell_print(sh, "last timeout   : none");
	}

	return 0;
}

static int cmd_hook_reg(const struct shell *sh, size_t argc, char **argv)
{
	long index;
	char *end;
	const char *text = argv[1];
	bool is_input = false;

	if ((argc < 2U) || (argc > 3U)) {
		shell_error(sh, "usage: hook reg <index|i<index>> [value]");
		return -EINVAL;
	}

	/* "hook reg i7" reads input register 7 (FC04 area, read only for the
	 * host), "hook reg 113" the holding register 113 (FC03 area).
	 */
	if ((text[0] == 'i') || (text[0] == 'I')) {
		is_input = true;
		text++;
	}

	index = strtol(text, &end, 0);
	if ((*end != '\0') || (index < 0) || (index >= (long)HOOK_REGISTER_COUNT)) {
		shell_error(sh, "register index out of range (0..%u)",
			    HOOK_REGISTER_COUNT - 1U);
		return -EINVAL;
	}

	if (argc == 2U) {
		uint16_t value = is_input ? hook_reg_input_get((uint16_t)index) :
					    hook_reg_get((uint16_t)index);

		shell_print(sh, "%s[%ld] = %u (0x%04X)", is_input ? "input" : "reg",
			    index, value, value);
		return 0;
	}

	{
		unsigned long value = strtoul(argv[2], &end, 0);

		if (*end != '\0') {
			shell_error(sh, "invalid value '%s'", argv[2]);
			return -EINVAL;
		}

		if (is_input) {
			hook_reg_input_set((uint16_t)index, (uint16_t)value);
			shell_print(sh, "input[%ld] <= %u (0x%04X)", index,
				    hook_reg_input_get((uint16_t)index),
				    hook_reg_input_get((uint16_t)index));
		} else {
			hook_reg_set((uint16_t)index, (uint16_t)value);
			shell_print(sh, "reg[%ld] <= %u (0x%04X)", index,
				    hook_reg_get((uint16_t)index),
				    hook_reg_get((uint16_t)index));
		}
	}

	return 0;
}

#define HOOK_SHELL_AT_LINE_MAX 128U
#define HOOK_USR_PREFIX "usr.cn#"

/* --- RTC bring-up helpers (no GPS on this product) ------------------ */

static bool hook_shell_parse_long(const char **text, long min, long max,
				  long *out)
{
	char *end;
	long value;

	if ((text == NULL) || (*text == NULL) || (out == NULL)) {
		return false;
	}

	value = strtol(*text, &end, 10);
	if ((end == *text) || (value < min) || (value > max)) {
		return false;
	}

	*text = end;
	*out = value;

	return true;
}

/*
 * hook rtc                                  - show the time state
 * hook rtc set <YYYY-MM-DD> <HH:MM>          - write the calibration registers
 *
 * "set" only fills the one shot calibration registers (holding 119/120/121),
 * exactly like a host Modbus write or the cloud 0x09 command; the RTC thread
 * applies them within a second and re-creates the trust record. Bench tool,
 * not a permanent host interface.
 */
static int cmd_hook_rtc(const struct shell *sh, size_t argc, char **argv)
{
	struct rtc_time tm;

	if (argc == 1U) {
		shell_print(sh, "display I%u=%04u I%u=%04X I%u=%04X (Beijing)",
			    INPUT_RTC_YEAR, hook_reg_input_get(INPUT_RTC_YEAR),
			    INPUT_RTC_MONTH_DAY,
			    hook_reg_input_get(INPUT_RTC_MONTH_DAY),
			    INPUT_RTC_TIME_HHMM,
			    hook_reg_input_get(INPUT_RTC_TIME_HHMM));
		shell_print(sh, "command H%u=%04u H%u=%04X H%u=%04X",
			    RTC_CAL_YEAR, hook_reg_get(RTC_CAL_YEAR),
			    RTC_CAL_MONTH_DAY, hook_reg_get(RTC_CAL_MONTH_DAY),
			    RTC_CAL_HHMM, hook_reg_get(RTC_CAL_HHMM));
		shell_print(sh, "trusted : %s",
			    hook_rtc_trust_is_valid() ? "yes" : "no");
		if (rtc_get_time(HOOK_SHELL_RTC, &tm) == 0) {
			shell_print(sh, "internal: %04d-%02d-%02d %02d:%02d:%02d UTC",
				    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
				    tm.tm_hour, tm.tm_min, tm.tm_sec);
		}
		return 0;
	}

	if ((argc != 4U) || (strcmp(argv[1], "set") != 0)) {
		shell_error(sh, "usage: hook rtc | hook rtc set <YYYY-MM-DD> <HH:MM>");
		return -EINVAL;
	}

	{
		const char *p = argv[2];
		long year;
		long month;
		long day;
		long hour;
		long minute;

		if (!hook_shell_parse_long(&p, 2000, 2099, &year) || (*p != '-')) {
			shell_error(sh, "invalid date '%s' (expected YYYY-MM-DD)", argv[2]);
			return -EINVAL;
		}
		p++;
		if (!hook_shell_parse_long(&p, 1, 12, &month) || (*p != '-')) {
			shell_error(sh, "invalid date '%s' (expected YYYY-MM-DD)", argv[2]);
			return -EINVAL;
		}
		p++;
		if (!hook_shell_parse_long(&p, 1, 31, &day) || (*p != '\0')) {
			shell_error(sh, "invalid date '%s' (expected YYYY-MM-DD)", argv[2]);
			return -EINVAL;
		}

		p = argv[3];
		if (!hook_shell_parse_long(&p, 0, 23, &hour) || (*p != ':')) {
			shell_error(sh, "invalid time '%s' (expected HH:MM)", argv[3]);
			return -EINVAL;
		}
		p++;
		if (!hook_shell_parse_long(&p, 0, 59, &minute) || (*p != '\0')) {
			shell_error(sh, "invalid time '%s' (expected HH:MM)", argv[3]);
			return -EINVAL;
		}

		/* Year last: the RTC thread consumes the triple as soon as all
		 * three registers differ from their "not written" default.
		 */
		hook_reg_set(RTC_CAL_MONTH_DAY,
			     (uint16_t)(((uint16_t)month << 8) | (uint16_t)day));
		hook_reg_set(RTC_CAL_HHMM,
			     (uint16_t)(((uint16_t)hour << 8) | (uint16_t)minute));
		hook_reg_set(RTC_CAL_YEAR, (uint16_t)year);
	}

	shell_print(sh, "calibration written, the RTC thread applies it in <1 s");

	return 0;
}

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
 * Expand the escapes a shell line cannot carry literally: \r, \n and \\.
 *
 * Needed for module parameters whose value contains line ends.
 * The expansion happens in place: the result is never longer than the input.
 */
static size_t hook_shell_expand_escapes(char *line, size_t len, size_t max_len)
{
	size_t out = 0U;
	size_t in = 0U;

	while ((in < len) && (out < max_len)) {
		if ((line[in] == '\\') && ((in + 1U) < len)) {
			char next = line[in + 1U];

			if (next == 'r') {
				line[out++] = '\r';
				in += 2U;
				continue;
			}
			if (next == 'n') {
				line[out++] = '\n';
				in += 2U;
				continue;
			}
			if (next == '\\') {
				line[out++] = '\\';
				in += 2U;
				continue;
			}
		}

		line[out++] = line[in++];
	}

	return out;
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

	/*
	 * "hook usr raw <data>": send plain data to the module with no "usr.cn#"
	 * command prefix and no AT answer collection.
	 *
	 * Needed to drive modes where the serial data itself is the trigger - in
	 * HTTPD mode the module turns plain serial data into the HTTP request,
	 * while anything starting with the command word is still parsed as an AT
	 * command and never reaches the server.
	 */
	if (strcmp(argv[1], "raw") == 0) {
		size_t raw_len;

		if (argc < 3U) {
			shell_error(sh, "usage: hook usr raw <data>");
			return -EINVAL;
		}

		raw_len = hook_shell_build_line(line, sizeof(line), NULL, argc - 1U,
						&argv[1]);
		if ((raw_len == 0U) || (raw_len > (sizeof(line) - 3U))) {
			shell_error(sh, "raw data too long");
			return -EINVAL;
		}

		raw_len = hook_shell_expand_escapes(line, raw_len,
						    sizeof(line) - 3U);

		/*
		 * "@@" marks a CR LF: a shell line cannot carry a real CR LF in
		 * this position. This is what lets raw data complete an HTTP
		 * request line in HTTPD mode:
		 *
		 *   hook usr raw " HTTP/1.1@@Host:1.2.3.4@@@@"
		 */
		{
			size_t out = 0U;

			for (size_t i = 0U; i < raw_len; i++) {
				if ((line[i] == '@') && ((i + 1U) < raw_len) &&
				    (line[i + 1U] == '@')) {
					line[out++] = '\r';
					line[out++] = '\n';
					i++;
					continue;
				}

				line[out++] = line[i];
			}

			raw_len = out;
		}

		line[raw_len++] = '\r';
		line[raw_len++] = '\n';
		line[raw_len] = '\0';

		if (hook_4g_send((const uint8_t *)line, (uint16_t)raw_len) < 0) {
			shell_error(sh, "could not send raw data");
			return -EIO;
		}

		shell_print(sh, "sent %u raw byte(s)", (unsigned int)raw_len);
		return 0;
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

	/* Allow \r / \n inside a parameter value. */
	used = hook_shell_expand_escapes(line, used, sizeof(line) - 3U);

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

	if (hook_ota_confirm() != 0) {
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

/*
 * Run the 4G provisioning flow by hand.
 *
 *   hook provision status   - what the module would be configured with
 *   hook provision run      - compare and write only what differs
 *   hook provision factory  - AT+CLEAR + full rewrite (new/swapped module)
 *
 * Without an argument "run" is used. The module restarts during the flow, so
 * the command blocks for 10~40 s.
 */
static int cmd_hook_provision(const struct shell *sh, size_t argc, char **argv)
{
	char summary[192];
	bool factory = false;
	int ret;

	if ((argc >= 2U) && (strcmp(argv[1], "status") == 0)) {
		(void)hook_4g_cfg_format_summary(summary, sizeof(summary));
		shell_print(sh, "state=%s",
			    hook_4g_cfg_state_name(hook_4g_cfg_get_state()));
		shell_print(sh, "%s", summary);
		return 0;
	}

	if ((argc >= 2U) && (strcmp(argv[1], "factory") == 0)) {
		factory = true;
	} else if ((argc >= 2U) && (strcmp(argv[1], "run") != 0)) {
		shell_error(sh, "usage: hook provision [status|run|factory]");
		return -EINVAL;
	}

	(void)hook_4g_cfg_format_summary(summary, sizeof(summary));
	shell_print(sh, "%s", summary);
	shell_print(sh, "provisioning%s, the module restarts, this takes 10-40 s...",
		    factory ? " (factory: clear + full rewrite)" : "");

	ret = hook_4g_cfg_run(factory);

	if (ret == 0) {
		shell_print(sh, "provisioning done, MQTT connected");
	} else {
		shell_error(sh, "provisioning failed: %d (state=%s)", ret,
			    hook_4g_cfg_state_name(hook_4g_cfg_get_state()));
	}

	return ret;
}

/*
 * "hook ota http <url>": start an HTTP firmware download.
 *
 * Same path the cloud triggers with "craner#AT+OTADL=<url>": the module
 * switches to HTTPD, the image is streamed into slot1, the module switches back
 * to MQTT and the controller keeps running until "hook ota test".
 */
static int cmd_hook_ota_http(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc < 2U) {
		shell_error(sh, "usage: hook ota http <http://host[:port]/path>");
		return -EINVAL;
	}

	ret = hook_ota_http_request(argv[1], (uint32_t)strlen(argv[1]));
	if (ret != 0) {
		shell_error(sh, "HTTP OTA request rejected: %d", ret);
		return ret;
	}

	shell_print(sh, "HTTP OTA queued: %s", argv[1]);
	shell_print(sh, "the 4G module switches to HTTPD; MQTT returns when it is");
	shell_print(sh, "over. Watch the log for 'HTTP OTA:' progress.");

	return 0;
}

/*
 * "hook ota test": same as "craner#AT+OTATEST" - arm the MCUboot test
 * upgrade for the downloaded image and reboot.
 */
static int cmd_hook_ota_test(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (hook_ota_http_status() != HOOK_OTA_HTTP_STAGED) {
		shell_error(sh, "no downloaded image is staged (state=%u)",
			    hook_ota_http_status());
		return -EINVAL;
	}

	shell_print(sh, "arming the MCUboot test upgrade, rebooting...");
	(void)hook_ota_test_and_reboot();

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
	SHELL_CMD_ARG(http, NULL,
		      "Download a firmware over HTTP: ota http <url>",
		      cmd_hook_ota_http, 2, 1),
	SHELL_CMD(test, NULL,
		  "Install the downloaded image (MCUboot test) and reboot.",
		  cmd_hook_ota_test),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	hook_cmds,
	SHELL_CMD(status, NULL, "Show hook controller status.", cmd_hook_status),
	SHELL_CMD_ARG(reg, NULL, "Read/write a Modbus register: reg <index> [value]",
		      cmd_hook_reg, 2, 1),
	SHELL_CMD_ARG(rtc, NULL,
		      "RTC state / host calibration: rtc [set <YYYY-MM-DD> <HH:MM>]",
		      cmd_hook_rtc, 1, 3),
	SHELL_CMD_ARG(link, NULL,
		      "4G link quality from the host heartbeat: link [clear]",
		      cmd_hook_link, 1, 1),
	SHELL_CMD_ARG(craner, NULL, "Run a craner#AT command: craner <command>",
		      cmd_hook_craner, 2, 8),
	SHELL_CMD_ARG(usr, NULL,
		      "4G module command: usr <AT command|shortcut>  "
		      "(shortcuts: wk_mod, mqtt_sta, mqtt_pub_tp, mqtt_sub_tp, "
		      "mqtt_svr, mqtt_user, mqtt_psw, mqtt_cid; add \"hex\" to "
		      "dump the frames)",
		      cmd_hook_usr, 2, 8),
	SHELL_CMD_ARG(provision, NULL,
		      "4G module provisioning: provision [status|run|factory]",
		      cmd_hook_provision, 1, 1),
	SHELL_CMD(ota, &hook_ota_cmds, "OTA image commands.", NULL),
	SHELL_CMD(reset, NULL, "Cold reboot the controller.", cmd_hook_reset),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(hook, &hook_cmds, "Craner AI safety hook commands.", NULL);
