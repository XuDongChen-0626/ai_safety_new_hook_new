/*
 * hook_4g_cfg.h - automatic 4G module provisioning (MQTT topics / mode).
 *
 * Replaces the manual factory procedure (a long list of "usr.cn#AT+..."
 * lines) with a boot time flow:
 *
 *   - the MQTT topics are derived from the persistent device.no parameter
 *     ("ais" + device.no), so every device configures itself;
 *   - the module is only rewritten when something actually differs, so a
 *     normal boot costs one or two AT queries and no flash write;
 *   - a module that is not in MQTT mode (new device, module swapped, someone
 *     reconfigured it) goes through AT+CLEAR + full rewrite.
 *
 * Everything runs over the synchronous hook_4g_at_cmd() channel, so the
 * exchange - which carries the MQTT password - is neither printed on the
 * shell nor mirrored to MQTT topic 1.
 */
#ifndef HOOK_4G_CFG_H
#define HOOK_4G_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hook_4g_cfg_state {
	HOOK_4G_CFG_IDLE = 0,
	HOOK_4G_CFG_WAIT_MODEM,
	HOOK_4G_CFG_READ,
	HOOK_4G_CFG_WRITE,
	HOOK_4G_CFG_VERIFY,
	HOOK_4G_CFG_DONE,
	HOOK_4G_CFG_FAILED,
};

/* Create the boot time provisioning thread (called from hook_task.c). */
void hook_4g_cfg_start(void);

/*
 * Run the provisioning flow synchronously.
 *
 * @param factory true: force AT+CLEAR + full rewrite (factory / module swap),
 *                false: compare first and only write what differs.
 *
 * @return 0 on success (module configured, MQTT connected).
 */
int hook_4g_cfg_run(bool factory);

enum hook_4g_cfg_state hook_4g_cfg_get_state(void);
const char *hook_4g_cfg_state_name(enum hook_4g_cfg_state state);

/* MQTT client id built from device.no, e.g. "ais998-hook". */
const char *hook_4g_cfg_cid(void);

/*
 * Switch the module to HTTPD mode so it can download a file, then back to
 * MQTT once the download is over.
 *
 * Both run over the synchronous AT channel and restart the module (AT+S),
 * which is unavoidable: the module only applies a new work mode after a
 * restart. The caller (app/hook_ota) is responsible for muting business
 * traffic via hook_4g_set_http_mode() while HTTPD is active.
 */
int hook_4g_cfg_enter_httpd(const char *host, uint16_t port, const char *path,
			    uint32_t http_timeout_s);
int hook_4g_cfg_enter_mqtt(void);

/* One line summary of the expected configuration (for "hook provision"). */
int hook_4g_cfg_format_summary(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_4G_CFG_H */
