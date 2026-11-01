/*
 * hook_alarm.h - external sound & light alarm module (uart7).
 *
 * Ported from application/app_bms_alarm.c (buzzer_logic / led_logic /
 * modbus_alarm_handle / power_on_self_test).
 */
#ifndef HOOK_ALARM_H
#define HOOK_ALARM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hook_alarm_start(void);

/*
 * Park/resume the alarm module handling.
 *
 * The host heartbeat supervision cuts the alarm supply (PE5) when the host
 * stops talking; an unpowered module cannot answer Modbus requests, so the
 * controller must stop talking to it at the same moment and only resume once
 * the supply is back. While suspended the module logic is skipped entirely
 * (the thread keeps checking in with the health service) and the cached
 * volume/LED state is invalidated, so the first exchange after the resume
 * re-pushes the real register values.
 */
void hook_alarm_set_suspend(bool suspended);

/* Force the volume register to be pushed to the module again. */
void hook_alarm_force_volume_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_ALARM_H */
