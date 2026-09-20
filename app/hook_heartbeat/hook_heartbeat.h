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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hook_heartbeat_init(void);
void hook_heartbeat_start(void);

/* True while the alarm-light supply (PE5) is enabled. */
bool hook_heartbeat_alarm_power_is_on(void);

/*
 * Host heartbeat reception quality (4G link quality).
 *
 * The crane host controller writes the heartbeat counter once per second, so
 * the gaps between two received heartbeats describe how stable the 4G/MQTT
 * downlink is: a gap longer than CONFIG_HOOK_LINK_TIMEOUT_MS counts as one
 * timeout and is written to the offline log.
 */
struct hook_link_stats {
	uint32_t received;           /* heartbeats received since boot/clear   */
	uint32_t timeouts;           /* gaps longer than the link timeout      */
	uint32_t interval_min_ms;    /* shortest gap between two heartbeats    */
	uint32_t interval_avg_ms;    /* average gap                            */
	uint32_t interval_max_ms;    /* longest gap                            */
	uint32_t timeout_max_ms;     /* longest gap counted as a timeout       */
	uint32_t last_timeout_ms;    /* uptime of the last timeout (0 = none)  */
	int64_t last_timeout_unix_s; /* wall clock of the last timeout         */
};

void hook_heartbeat_get_link_stats(struct hook_link_stats *out);
void hook_heartbeat_clear_link_stats(void);

/*
 * Suspend/resume the heartbeat timeout actions.
 *
 * While the 4G module is being provisioned it saves its configuration and
 * restarts, so no heartbeat can arrive for 10~40 s. The counter keeps being
 * followed, only the consequences (LED/buzzer cut, alarm supply off, offline
 * detection) are parked - resuming re-arms the timers so the first heartbeat
 * after the provisioning does not trip anything.
 */
#if defined(CONFIG_HOOK_HOST_HEARTBEAT_ENABLE)
void hook_heartbeat_set_suspend(bool suspended);
#else
static inline void hook_heartbeat_set_suspend(bool suspended)
{
	(void)suspended;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* HOOK_HEARTBEAT_H */
