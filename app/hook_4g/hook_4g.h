/*
 * hook_4g.h - 4G / cloud channel (usr.cn transparent module on usart1).
 *
 * Ported from application/uart_manage_handler.c and application/app_4g.c:
 *   - "<n>," prefixes added by the usr.cn module
 *   - craner#AT command surface (see hook_at.h)
 *   - binary 0xA5 cloud protocol (see hook_cloud_proto.h)
 *   - hourly "usr.cn#AT+Z" keepalive
 */
#ifndef HOOK_4G_H
#define HOOK_4G_H

#include <stdbool.h>
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
 */
void hook_4g_at_exchange_start(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_4G_H */
