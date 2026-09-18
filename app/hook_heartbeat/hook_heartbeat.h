/*
 * hook_heartbeat.h - host heartbeat supervision and alarm-light power.
 *
 * Ported from application/app_relay.c. There is no relay on this board:
 * PE5 (FLASH_LIGHT_POWER_EN in the .ioc / "警报灯供电使能" on the schematic)
 * is the supply enable of the sound & light alarm, so the module is named
 * after what it actually does.
 *
 * The crane host controller increments holding register CMD_HOST_HEARTBEAT
 * periodically; this module watches that counter.
 */
#ifndef HOOK_HEARTBEAT_H
#define HOOK_HEARTBEAT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hook_heartbeat_init(void);
void hook_heartbeat_start(void);

/* True while the alarm-light supply (PE5) is enabled. */
bool hook_heartbeat_alarm_power_is_on(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_HEARTBEAT_H */
