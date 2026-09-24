/*
 * hook_4g.h - 4G / cloud channel (usr.cn transparent module on usart1).
 *
 * Ported from application/uart_manage_handler.c and application/app_4g.c:
 *   - "<n>," prefixes added by the usr.cn module
 *   - craner#AT command surface (see hook_at.h)
 *   - binary 0xA5 cloud protocol (see hook_cloud_proto.h)
 *
 * The old firmware also sent an hourly "usr.cn#AT+Z" link keep-alive; it is
 * gone because the module restarts on that command (see hook_4g.c).
 */
#ifndef HOOK_4G_H
#define HOOK_4G_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hook_4g_init(void);
void hook_4g_start(void);

/*
 * Send raw bytes to the 4G module. All writes to the usr.cn link are
 * serialised here because the 4G thread and the OTA thread both transmit
 * (uart_manage itself is not re-entrant).
 */
int hook_4g_send(const uint8_t *data, uint16_t len);

/* Send on usr.cn channel 2 (OTA data / progress): adds the "2," prefix. */
int hook_4g_send_ota(const uint8_t *data, uint16_t len);

/*
 * Debug aid: while enabled every frame received from the module is dumped as
 * hex (8 bytes per log line) in addition to the normal handling, and the
 * receive counters are reset so the dump covers exactly the next command.
 * Used by "hook usr hex on|off|<command>".
 */
void hook_4g_enable_hex_echo(bool enable);

/*
 * Arm the module AT answer collector. Called whenever a "usr.cn#..." command
 * is sent to the module: the answer is reassembled (the UART idle timer may
 * split it over several chunks) until its "OK"/"ERROR" terminator and then
 * printed on the shell and published on topic 1.
 *
 * Returns -EBUSY when a synchronous exchange (hook_4g_at_cmd) owns the AT
 * channel, so a hand typed command cannot interleave with it.
 */
int hook_4g_at_exchange_start(void);

/*
 * Synchronous AT exchange: send "usr.cn#<cmd>" and wait for the module answer.
 *
 * Unlike the shell passthrough this does NOT print the exchange on the shell
 * and does NOT mirror it to MQTT topic 1, which matters for the automatic
 * provisioning flow (it sends the MQTT password).
 *
 * @param cmd        command without the "usr.cn#" prefix, e.g. "AT+MQTTSTA"
 * @param resp       optional buffer for the whole answer (NUL terminated)
 * @param resp_len   size of resp
 * @param timeout_ms how long to wait for the OK/ERROR terminator
 *
 * @return 0 on "OK", -EIO on "ERROR", -ETIMEDOUT, -EBUSY when the AT channel
 *         is busy, -EINVAL on bad arguments.
 */
int hook_4g_at_cmd(const char *cmd, char *resp, size_t resp_len,
		   uint32_t timeout_ms);

/* True while hook_4g_at_cmd() owns the AT channel. */
bool hook_4g_at_cmd_is_active(void);

/*
 * Module boot banner ("WH-LTE-7S1") observed on the link.
 *
 * The module must not be talked to while it is still booting: the automatic
 * provisioning waits for this flag before its first probe. Builds that hide
 * the banner are covered by a bounded fallback wait.
 */
bool hook_4g_modem_boot_seen(void);
void hook_4g_modem_boot_reset(void);

/*
 * Hard reset of the module by cycling its supply rail.
 *
 * This is the recovery action when the module is unresponsive as a whole (no
 * AT answer, or a stuck MQTT client): AT+S and AT+CLEAR only ask the module to
 * restart itself, which a stuck module cannot do. The controller keeps
 * running, so nothing in RAM (staged image, OTA progress) is lost.
 *
 * Callers must wait for the module to boot afterwards (cfg_wait_modem()).
 */
void hook_4g_modem_hard_reset(void);

/*
 * HTTPD mode.
 *
 * While the module is downloading a file over HTTP (AT+WKMOD=HTTPD), every
 * byte it receives on the serial line is appended to the HTTP request it is
 * building, and everything it gets back is the response body. So in this mode:
 *
 *   - business transmissions (heartbeat publishes, AT answer mirroring, OTA
 *     progress on "2,") are muted - they would corrupt the request;
 *   - received data is handed to the HTTP OTA download session instead of the
 *     0xA5 protocol parser;
 *   - the synchronous AT channel keeps working, because switching back to
 *     MQTT needs it.
 */
void hook_4g_set_http_mode(bool on);
bool hook_4g_is_http_mode(void);

/*
 * Trigger one HTTP request in HTTPD mode by sending the request tail the
 * module does not build itself (version + Host header + terminating blank
 * line). The module prepends "GET <AT+HTPURL> ".
 *
 * The Host header carries the port when it is not 80: the server is reached on
 * that port (AT+HTPSV), and a Host without it makes virtual-host routing pick
 * the wrong default.
 */
int hook_4g_http_trigger(const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_4G_H */
