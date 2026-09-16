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

/* Force the volume register to be pushed to the module again. */
void hook_alarm_force_volume_sync(void);

/* True while the module answered the last LED command. */
bool hook_alarm_led_is_online(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_ALARM_H */
