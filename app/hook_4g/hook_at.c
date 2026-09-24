/*
 * hook_at.c - craner#AT command surface.
 */
#include "hook_at.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "ota_manager_service.h"
#include "hook_4g.h"
#include "hook_heartbeat.h"
#include "hook_ota.h"
#include "hook_params.h"
#include "uart_manage.h"
#include "uart_manage_port.h"

LOG_MODULE_REGISTER(hook_at, CONFIG_LOG_DEFAULT_LEVEL);

typedef int32_t (*hook_at_cmd_handler_t)(const uint8_t *cmd, uint16_t len,
					 hook_at_reply_fn_t reply_fn);

typedef struct {
	const char *cmd;
	hook_at_cmd_handler_t handler;
} hook_at_cmd_entry_t;

typedef struct {
	const uint8_t *full_cmd;
	uint16_t full_cmd_len;
	const uint8_t *cmd;
	uint16_t cmd_len;
} hook_at_command_t;

/* Reboot is deferred so the "craner#OK" reply is on the wire first. */
#define HOOK_AT_REBOOT_DELAY_MS 200U

static uint8_t hook_at_is_blank(uint8_t ch)
{
	return (uint8_t)((ch == ' ') || (ch == '\t') || (ch == '\r') ||
			 (ch == '\n'));
}

static int32_t hook_at_find_command(const uint8_t *buf, uint16_t len,
				    hook_at_command_t *at_cmd)
{
	static const char hook_prefix[] = "craner#";
	const uint16_t prefix_len = (uint16_t)(sizeof(hook_prefix) - 1U);
	uint16_t index = 0U;
	uint16_t full_cmd_len;

	if ((buf == NULL) || (at_cmd == NULL)) {
		return HOOK_AT_PREFIX_NOT_MATCH;
	}

	while ((index < len) && (hook_at_is_blank(buf[index]) != 0U)) {
		index++;
	}

	if ((uint32_t)(len - index) < prefix_len) {
		return HOOK_AT_PREFIX_NOT_MATCH;
	}

	if (memcmp(&buf[index], hook_prefix, prefix_len) != 0) {
		return HOOK_AT_PREFIX_NOT_MATCH;
	}

	full_cmd_len = (uint16_t)(len - index);
	while ((full_cmd_len > prefix_len) &&
	       (hook_at_is_blank(buf[index + full_cmd_len - 1U]) != 0U)) {
		full_cmd_len--;
	}

	at_cmd->full_cmd = &buf[index];
	at_cmd->full_cmd_len = full_cmd_len;
	at_cmd->cmd = &buf[index + prefix_len];
	at_cmd->cmd_len = (uint16_t)(full_cmd_len - prefix_len);

	return HOOK_AT_OK;
}

static void hook_at_reply(hook_at_reply_fn_t reply_fn, const char *text)
{
	if (reply_fn != NULL) {
		(void)reply_fn((const uint8_t *)text, (uint16_t)strlen(text));
	}
}

static void hook_at_reply_ok(hook_at_reply_fn_t reply_fn)
{
	hook_at_reply(reply_fn, "craner#OK\r\n");
}

static void hook_at_reply_error(hook_at_reply_fn_t reply_fn)
{
	hook_at_reply(reply_fn, "craner#ERROR\r\n");
}

static void hook_at_reply_cmd_error(hook_at_reply_fn_t reply_fn,
				    const uint8_t *cmd, uint16_t len)
{
	char text[96];
	const uint8_t *body = cmd;
	uint16_t body_len = len;
	int written;

	if ((len > 2U) && (cmd[0] == 'A') && (cmd[1] == 'T')) {
		body = &cmd[2];
		body_len = (uint16_t)(len - 2U);
	}

	/* One single reply buffer: the console path erases the current line on
	 * every call, so a split answer would be mangled.
	 */
	if (body_len > (sizeof(text) - 16U)) {
		body_len = (uint16_t)(sizeof(text) - 16U);
	}

	written = snprintf(text, sizeof(text), "craner#%.*s ERROR\r\n", (int)body_len,
			   (const char *)body);
	if ((written > 0) && (reply_fn != NULL)) {
		(void)reply_fn((const uint8_t *)text, (uint16_t)written);
	}
}

static int32_t hook_at_cmd_ping(const uint8_t *cmd, uint16_t len,
				hook_at_reply_fn_t reply_fn)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(len);

	hook_at_reply_ok(reply_fn);

	return HOOK_AT_OK;
}

int hook_at_format_image_info(char *buf, size_t buf_len)
{
	struct ota_manager_image_info info;
	int written = 0;

	if ((buf == NULL) || (buf_len == 0U)) {
		return -1;
	}

	for (uint8_t slot = 0U; slot < 2U; slot++) {
		int ret = ota_manager_get_image_info(slot, &info);
		int part;

		if (ret != 0 || !info.valid) {
			part = snprintf(&buf[written], buf_len - (size_t)written,
					"%s:slot%u=empty", (slot == 0U) ? "" : ",",
					slot);
		} else {
			part = snprintf(&buf[written], buf_len - (size_t)written,
					"%s:slot%u=%s%s%s%s", (slot == 0U) ? "" : ",",
					slot, info.version,
					((info.state_flags &
					  OTA_MANAGER_IMAGE_STATE_ACTIVE) != 0U) ?
						",active" : "",
					((info.state_flags &
					  OTA_MANAGER_IMAGE_STATE_PENDING) != 0U) ?
						",pending" : "",
					((info.state_flags &
					  OTA_MANAGER_IMAGE_STATE_CONFIRMED) != 0U) ?
						",confirmed" : "");
		}

		if (part <= 0) {
			return -1;
		}
		written += part;
	}

	return written;
}

static int32_t hook_at_cmd_ota_info(const uint8_t *cmd, uint16_t len,
				    hook_at_reply_fn_t reply_fn)
{
	char info[96];
	char line[288];
	char staged[OTA_MANAGER_IMAGE_VERSION_MAX_LEN + 2U] = "-";
	int written;

	ARG_UNUSED(cmd);
	ARG_UNUSED(len);

	if (hook_at_format_image_info(info, sizeof(info)) <= 0) {
		hook_at_reply_error(reply_fn);
		return HOOK_AT_ACTION_FAILED;
	}

	/* Version of the image that is waiting for AT+OTATEST, if any. */
	if (hook_ota_staged_pending()) {
		struct ota_manager_image_info staged_info;

		if ((ota_manager_get_image_info(1U, &staged_info) == 0) &&
		    staged_info.valid) {
			(void)snprintf(staged, sizeof(staged), "v%s",
				       staged_info.version);
		}
	}

	/*
	 * Image information plus both OTA session states in one reply (a single
	 * console write, otherwise the shell line editor mangles split output):
	 *
	 *   +OTAHTTP:<0 idle|1 downloading|2 ready|3 error>,<written>,<expected>
	 *     - the HTTP download requested with AT+OTADL
	 *   +OTASTATE:<state>,<received>,<size>
	 *     - the Ymodem session requested with AT+OTAREQUEST
	 *   +OTASTAGED:<0 no|1 yes>,v<version>
	 *     - slot1 holds a verified image waiting for AT+OTATEST, also
	 *       after a reset (the persisted ota.staged marker)
	 */
	written = snprintf(line, sizeof(line),
			   "craner#+OTAINFO%s\r\n"
			   "craner#+OTAHTTP:%u,%u,%u\r\n"
			   "craner#+OTASTATE:%s,%u,%u\r\n"
			   "craner#+OTASTAGED:%u,%s\r\n",
			   info, (unsigned int)hook_ota_http_status(),
			   hook_ota_http_bytes(), hook_ota_http_expect_bytes(),
			   hook_ota_state_name(hook_ota_get_state()),
			   hook_ota_bytes_received(), hook_ota_image_size(),
			   hook_ota_staged_pending() ? 1U : 0U,
			   staged);
	if ((written <= 0) || ((size_t)written >= sizeof(line))) {
		hook_at_reply_error(reply_fn);
		return HOOK_AT_ACTION_FAILED;
	}

	hook_at_reply(reply_fn, line);

	return HOOK_AT_OK;
}

static int32_t hook_at_cmd_ota_request(const uint8_t *cmd, uint16_t len,
				       hook_at_reply_fn_t reply_fn)
{
	int ret;

	ARG_UNUSED(cmd);
	ARG_UNUSED(len);

	/* Arm the Ymodem receiver; the host tool waits for our 'C'. */
	ret = hook_ota_request();
	if (ret != 0) {
		LOG_ERR("OTA request rejected: %d", ret);
		hook_at_reply_error(reply_fn);
		return HOOK_AT_ACTION_FAILED;
	}

	hook_at_reply_ok(reply_fn);

	return HOOK_AT_OK;
}

static int32_t hook_at_cmd_ota_cancel(const uint8_t *cmd, uint16_t len,
				      hook_at_reply_fn_t reply_fn)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(len);

	LOG_INF("OTA cancel requested by host");
	hook_ota_cancel();
	hook_at_reply_ok(reply_fn);

	return HOOK_AT_OK;
}

/* Manual confirmation of the running image (engineer driven). */
static int32_t hook_at_cmd_ota_confirm(const uint8_t *cmd, uint16_t len,
				       hook_at_reply_fn_t reply_fn)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(len);

	if (hook_ota_confirm() == 0) {
		LOG_INF("running image confirmed");
		hook_at_reply_ok(reply_fn);
		return HOOK_AT_OK;
	}

	hook_at_reply_cmd_error(reply_fn, cmd, len);

	return HOOK_AT_ACTION_FAILED;
}

static int32_t hook_at_cmd_ota_boot(const uint8_t *cmd, uint16_t len,
				    hook_at_reply_fn_t reply_fn)
{
	int ret;

	ARG_UNUSED(cmd);
	ARG_UNUSED(len);

	/*
	 * The host tool sends "craner#AT+OTABOOT" to start an update and then
	 * waits for the Ymodem 'C'. The download happens inside the running
	 * application (slot 1 is written while slot 0 keeps executing), so no
	 * reboot into a bootloader is needed.
	 */
	ret = hook_ota_request();
	if (ret != 0) {
		LOG_ERR("OTA start rejected: %d", ret);
		hook_at_reply_error(reply_fn);
		return HOOK_AT_ACTION_FAILED;
	}

	hook_at_reply_ok(reply_fn);

	return HOOK_AT_OK;
}

static int32_t hook_at_cmd_fw_time(const uint8_t *cmd, uint16_t len,
				   hook_at_reply_fn_t reply_fn)
{
	static const char fw_time[] = "craner#FWTIME:" __DATE__ " " __TIME__ "\r\n";

	ARG_UNUSED(cmd);
	ARG_UNUSED(len);

	hook_at_reply(reply_fn, fw_time);

	return HOOK_AT_OK;
}

static int32_t hook_at_cmd_sys_reset(const uint8_t *cmd, uint16_t len,
				     hook_at_reply_fn_t reply_fn)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(len);
	ARG_UNUSED(reply_fn);

	k_msleep(HOOK_AT_REBOOT_DELAY_MS);
	sys_reboot(SYS_REBOOT_COLD);

	return HOOK_AT_OK;
}

/*
 * Remote parameter access (cloud -> device over MQTT topic 1):
 *
 *   craner#AT+PARAM                    list every parameter (passwords masked)
 *   craner#AT+PARAM=<key>              read one parameter
 *   craner#AT+PARAM=<key>,<value>      write it (memory only, dirty)
 *   craner#AT+PARAM=<key>,<value>,save write and persist it
 *   craner#AT+PARAM=save               persist every dirty parameter
 *   craner#AT+PARAM=reset,<key>        restore the default (memory only)
 *
 * Writing never reconfigures the 4G module: a changed device.no or mqtt.*
 * value takes effect on the next boot, exactly like "param set" + "param
 * save" on the shell.
 */
#define HOOK_AT_PARAM_TEXT_MAX 640U
#define HOOK_AT_PARAM_ARGS_MAX 160U

static int32_t hook_at_param_error(hook_at_reply_fn_t reply_fn,
				   const char *reason)
{
	char text[64];
	int written = snprintf(text, sizeof(text), "craner#+PARAMERR:%s\r\n",
			       reason);

	if ((written > 0) && (reply_fn != NULL)) {
		(void)reply_fn((const uint8_t *)text, (uint16_t)written);
	}

	return HOOK_AT_ACTION_FAILED;
}

/* Append ";key=value" to the reply text (secret values masked). */
static size_t hook_at_param_append(char *buf, size_t buf_len, size_t used,
				   const char *key)
{
	char value[CONFIG_DEVICE_PARAM_SERVER_MAX_VALUE_LEN];
	const char *shown;
	int written;

	if (hook_params_get(key, value, sizeof(value)) != 0) {
		(void)snprintk(value, sizeof(value), "<n/a>");
	}

	shown = hook_params_key_is_secret(key) ? "***" : value;
	written = snprintf(&buf[used], buf_len - used, "%s%s=%s",
			   (used > 0U) ? ";" : "", key, shown);

	return (written > 0) ? (used + (size_t)written) : used;
}

static int32_t hook_at_param_reply(hook_at_reply_fn_t reply_fn, char *text,
				   size_t used, int32_t result)
{
	if ((result == HOOK_AT_OK) && (used < (HOOK_AT_PARAM_TEXT_MAX - 2U))) {
		text[used++] = '\r';
		text[used++] = '\n';
		text[used] = '\0';
	}

	if (reply_fn != NULL) {
		(void)reply_fn((const uint8_t *)text, (uint16_t)used);
	}

	return result;
}

static int32_t hook_at_param_list(hook_at_reply_fn_t reply_fn)
{
	static char text[HOOK_AT_PARAM_TEXT_MAX];
	size_t used;

	used = (size_t)snprintf(text, sizeof(text), "craner#+PARAM:%u",
				(unsigned int)hook_params_count());

	for (size_t i = 0U; hook_params_key_at(i) != NULL; i++) {
		if (used >= (sizeof(text) - 96U)) {
			break;
		}

		used = hook_at_param_append(text, sizeof(text) - 3U, used,
					    hook_params_key_at(i));
	}

	return hook_at_param_reply(reply_fn, text, used, HOOK_AT_OK);
}

static int32_t hook_at_param_read(hook_at_reply_fn_t reply_fn, const char *key)
{
	static char text[HOOK_AT_PARAM_TEXT_MAX];
	size_t used;

	used = (size_t)snprintf(text, sizeof(text), "craner#+PARAM:");
	used = hook_at_param_append(text, sizeof(text) - 3U, used, key);

	if (used <= strlen("craner#+PARAM:")) {
		return hook_at_param_error(reply_fn, "key");
	}

	return hook_at_param_reply(reply_fn, text, used, HOOK_AT_OK);
}

static int32_t hook_at_cmd_param(const uint8_t *cmd, uint16_t len,
				 hook_at_reply_fn_t reply_fn)
{
	char args[HOOK_AT_PARAM_ARGS_MAX];
	const uint8_t *eq = memchr(cmd, '=', len);
	size_t args_len;
	char *separator;
	char *value;
	bool save = false;
	int ret;

	/* No argument: dump every parameter. */
	if (eq == NULL) {
		return hook_at_param_list(reply_fn);
	}

	args_len = (size_t)(len - (uint16_t)(eq - cmd) - 1U);
	if ((args_len == 0U) || (args_len >= sizeof(args))) {
		return hook_at_param_error(reply_fn, "argument");
	}

	memcpy(args, &eq[1], args_len);
	args[args_len] = '\0';

	/* The command may still carry CR/LF from the MQTT frame. */
	while ((args_len > 0U) &&
	       (hook_at_is_blank((uint8_t)args[args_len - 1U]) != 0U)) {
		args[--args_len] = '\0';
	}

	if (strcmp(args, "save") == 0) {
		ret = hook_params_save();
		if (ret != 0) {
			LOG_ERR("remote param save failed: %d", ret);
			return hook_at_param_error(reply_fn, "save");
		}

		hook_at_reply_ok(reply_fn);
		return HOOK_AT_OK;
	}

	if (strncmp(args, "reset,", 6) == 0) {
		const char *key = &args[6];

		if (key[0] == '\0') {
			return hook_at_param_error(reply_fn, "key");
		}

		ret = hook_params_reset(key);
		if (ret != 0) {
			LOG_ERR("remote param reset '%s' failed: %d", key, ret);
			return hook_at_param_error(reply_fn, "reset");
		}

		hook_at_reply_ok(reply_fn);
		return HOOK_AT_OK;
	}

	separator = strchr(args, ',');
	if (separator == NULL) {
		return hook_at_param_read(reply_fn, args);
	}

	*separator = '\0';
	value = &separator[1];

	/* A trailing ",save" persists the write in one step. Comparison runs on
	 * the last comma so values containing ',' stay intact.
	 */
	{
		char *flag = strrchr(value, ',');

		if ((flag != NULL) && (strcmp(&flag[1], "save") == 0)) {
			*flag = '\0';
			save = true;
		}
	}

	ret = hook_params_set(args, value);
	if (ret != 0) {
		LOG_ERR("remote param set '%s' failed: %d", args, ret);
		return hook_at_param_error(reply_fn, "set");
	}

	if (save) {
		ret = hook_params_save();
		if (ret != 0) {
			LOG_ERR("remote param save after '%s' failed: %d", args,
				ret);
			return hook_at_param_error(reply_fn, "save");
		}
	}

	hook_at_reply_ok(reply_fn);

	return HOOK_AT_OK;
}

/*
 * craner#AT+OTADL=<url>
 *
 * Queue an HTTP firmware download (topic 1, so the cloud and the bench use the
 * same command). The module switches to HTTPD, MQTT comes back as soon as the
 * download is over, and the test upgrade is requested separately.
 */
static int32_t hook_at_cmd_ota_download(const uint8_t *cmd, uint16_t len,
					hook_at_reply_fn_t reply_fn)
{
	const uint8_t *url;
	uint16_t url_len;
	int ret;

	if ((len <= 8U) || (cmd[8] != '=')) {
		hook_at_reply_cmd_error(reply_fn, cmd, len);
		return HOOK_AT_ACTION_FAILED;
	}

	url = &cmd[9];
	url_len = (uint16_t)(len - 9U);

	/* The line may still carry CR/LF from the cloud frame. */
	while ((url_len > 0U) && (hook_at_is_blank(url[url_len - 1U]) != 0U)) {
		url_len--;
	}

	if (url_len == 0U) {
		hook_at_reply_cmd_error(reply_fn, cmd, len);
		return HOOK_AT_ACTION_FAILED;
	}

	ret = hook_ota_http_request((const char *)url, url_len);
	if (ret != 0) {
		LOG_WRN("AT+OTADL rejected: %d", ret);
		hook_at_reply_cmd_error(reply_fn, cmd, len);
		return HOOK_AT_ACTION_FAILED;
	}

	hook_at_reply_ok(reply_fn);

	return HOOK_AT_OK;
}

/*
 * craner#AT+OTATEST
 *
 * Install the image that a previous AT+OTADL download staged (MCUboot test
 * upgrade) and reboot. Kept separate from the download so the device keeps
 * running - and reachable over MQTT - until the install is asked for.
 */
static int32_t hook_at_cmd_ota_test(const uint8_t *cmd, uint16_t len,
				    hook_at_reply_fn_t reply_fn)
{
	ARG_UNUSED(cmd);
	ARG_UNUSED(len);

	/*
	 * slot1 can have been staged by the HTTP session (AT+OTADL), by the
	 * Ymodem session (AT+OTAREQUEST), or by an HTTP download that finished
	 * before the last reset (the persisted ota.staged marker).
	 */
	if (!hook_ota_test_available()) {
		LOG_WRN("AT+OTATEST: no downloaded image is staged (state=%u)",
			hook_ota_http_status());
		hook_at_reply_error(reply_fn);
		return HOOK_AT_ACTION_FAILED;
	}

	/* Acknowledge first: the reboot follows a short delay later. */
	hook_at_reply_ok(reply_fn);

	(void)hook_ota_test_and_reboot();

	return HOOK_AT_OK;
}

/*
 * craner#AT+LINK[=clear]
 *
 * Downlink quality of the 4G/MQTT link, derived from the arrival times of the
 * host heartbeat counter. This is an engineering measurement (how stable is
 * the downlink), not product data for the host application, so it lives on the
 * diagnostics channel next to the other AT commands.
 *
 *   AT+LINK        -> craner#+LINK:<received>,<timeouts>,<min>,<avg>,<max>,<longest timeout>   [ms]
 *   AT+LINK=clear  -> craner#OK
 */
static int32_t hook_at_cmd_link(const uint8_t *cmd, uint16_t len,
				hook_at_reply_fn_t reply_fn)
{
	struct hook_link_stats st;
	char line[128];
	int written;

	/* "AT+LINK" is 7 characters, so an argument starts at index 7. */
	if ((len > 7U) && (cmd[7] == '=')) {
		if ((len == 13U) && (memcmp(&cmd[8], "clear", 5U) == 0)) {
			hook_heartbeat_clear_link_stats();
			hook_at_reply_ok(reply_fn);
			return HOOK_AT_OK;
		}

		hook_at_reply_cmd_error(reply_fn, cmd, len);
		return HOOK_AT_ACTION_FAILED;
	}

	hook_heartbeat_get_link_stats(&st);

	written = snprintf(line, sizeof(line),
			   "craner#+LINK:%u,%u,%u,%u,%u,%u\r\n",
			   (unsigned int)st.received,
			   (unsigned int)st.timeouts,
			   (unsigned int)st.interval_min_ms,
			   (unsigned int)st.interval_avg_ms,
			   (unsigned int)st.interval_max_ms,
			   (unsigned int)st.timeout_max_ms);
	if ((written <= 0) || ((size_t)written >= sizeof(line))) {
		hook_at_reply_error(reply_fn);
		return HOOK_AT_ACTION_FAILED;
	}

	hook_at_reply(reply_fn, line);

	return HOOK_AT_OK;
}

static const hook_at_cmd_entry_t hook_at_cmd_table[] = {
	{"AT", hook_at_cmd_ping},
	{"AT+PARAM", hook_at_cmd_param},
	{"AT+LINK", hook_at_cmd_link},
	{"AT+OTADL", hook_at_cmd_ota_download},
	{"AT+OTATEST", hook_at_cmd_ota_test},
	{"AT+OTAINFO", hook_at_cmd_ota_info},
	{"AT+OTAREQUEST", hook_at_cmd_ota_request},
	{"AT+OTACANCEL", hook_at_cmd_ota_cancel},
	{"AT+OTA_CONFIRM", hook_at_cmd_ota_confirm},
	{"AT+OTABOOT", hook_at_cmd_ota_boot},
	{"AT+FWTIME", hook_at_cmd_fw_time},
	{"AT+SYSRESET", hook_at_cmd_sys_reset},
};

int32_t hook_at_craner_handler(const uint8_t *buf, uint16_t len,
			       hook_at_reply_fn_t reply_fn)
{
	hook_at_command_t at_cmd;

	if (hook_at_find_command(buf, len, &at_cmd) != HOOK_AT_OK) {
		return HOOK_AT_PREFIX_NOT_MATCH;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(hook_at_cmd_table); i++) {
		size_t table_len = strlen(hook_at_cmd_table[i].cmd);
		bool exact;
		bool with_args;

		if (at_cmd.cmd_len < table_len) {
			continue;
		}

		exact = (at_cmd.cmd_len == table_len) &&
			(memcmp(at_cmd.cmd, hook_at_cmd_table[i].cmd,
				table_len) == 0);
		/* "AT+PARAM=..." carries an argument after '='; plain "AT+..."
		 * commands such as "AT" never match a longer line because the
		 * next character has to be '='.
		 */
		with_args = (at_cmd.cmd_len > table_len) &&
			    (at_cmd.cmd[table_len] == '=') &&
			    (memcmp(at_cmd.cmd, hook_at_cmd_table[i].cmd,
				    table_len) == 0);

		if (exact || with_args) {
			return hook_at_cmd_table[i].handler(at_cmd.cmd, at_cmd.cmd_len,
							    reply_fn);
		}
	}

	hook_at_reply_error(reply_fn);

	return HOOK_AT_UNKNOWN_CMD;
}

int32_t hook_at_usr_handler(const uint8_t *buf, uint16_t len)
{
	static const char at_prefix[] = "usr.cn#AT";
	const uint16_t prefix_len = (uint16_t)(sizeof(at_prefix) - 1U);
	uint16_t index = 0U;

	while ((index < len) && (hook_at_is_blank(buf[index]) != 0U)) {
		index++;
	}

	if ((uint32_t)(len - index) < prefix_len) {
		return HOOK_AT_PREFIX_NOT_MATCH;
	}

	if (memcmp(&buf[index], at_prefix, prefix_len) != 0) {
		return HOOK_AT_PREFIX_NOT_MATCH;
	}

	/* Forward the line unchanged to the usr.cn module. */
	return hook_at_usr_forward(&buf[index], (uint16_t)(len - index));
}

int32_t hook_at_usr_forward(const uint8_t *line, uint16_t len)
{
	uint8_t last;

	if ((line == NULL) || (len == 0U)) {
		return HOOK_AT_PREFIX_NOT_MATCH;
	}

	/*
	 * Arm the answer collector first: the automatic provisioning flow holds
	 * the AT channel with synchronous exchanges, and a hand typed command
	 * must not be sent into the middle of one.
	 */
	if (hook_4g_at_exchange_start() != 0) {
		LOG_WRN("4G AT channel busy (automatic provisioning), command not sent");
		return HOOK_AT_ACTION_FAILED;
	}

	(void)hook_4g_send(line, len);

	/*
	 * usr.cn modules terminate an AT command line with CRLF. The cloud path
	 * forwarded the CRLF it received; a locally typed command has to get it
	 * appended here, otherwise the module never answers.
	 */
	last = line[len - 1U];

	if (last == '\n') {
		/* Line already complete. */
	} else if (last == '\r') {
		static const uint8_t lf[] = "\n";

		(void)hook_4g_send(lf, 1U);
	} else {
		static const uint8_t crlf[] = "\r\n";

		(void)hook_4g_send(crlf, 2U);
	}

	return HOOK_AT_OK;
}
