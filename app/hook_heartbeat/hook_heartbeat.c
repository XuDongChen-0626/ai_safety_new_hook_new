/*
 * hook_heartbeat.c - host heartbeat supervision.
 *
 * The crane host controller increments holding register CMD_HOST_HEARTBEAT
 * periodically. This module watches that counter and controls the alarm-light
 * supply (FLASH_LIGHT_POWER_EN / PE5):
 *
 *   heartbeat moving         -> alarm power on, ERR_HEARTBEAT_TIMEOUT cleared
 *   silence > 10 s           -> LED/buzzer commands cleared
 *   silence > 60 s           -> alarm power off + PWD_LED off (the CubeMX
 *                               firmware cut both pins at this point);
 *                               ERR_HEARTBEAT_TIMEOUT is set by the health
 *                               service callback at the same 60 s threshold,
 *                               see app/hook_health
 *   HEARTBEAT_ENABLE (117)=0 -> supervision disabled, power stays on
 *   soft standby             -> power off, timers kept "fed"
 *
 * Only the alarm-light supply is driven here. The PWD_LED (PB3) is driven by
 * app/hook_alarm (it mirrors the Modbus flash-light command register) and by
 * the soft standby / POST paths, which is the behaviour of the latest CubeMX
 * firmware.
 *
 * The module records its liveness through hook_health_task_alive() on every
 * loop pass, including while it only idles: a check-in means "the thread is
 * running", not "the thread had work to do" (see hook_health.h).
 */
#include "hook_heartbeat.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "board.h"
#include "hook_alarm.h"
#include "hook_health.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "offline_log_service.h"
#include "osal_zephyr.h"
#include "system_def.h"
#include "time_manager_service.h"

LOG_MODULE_REGISTER(hook_heartbeat, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_HEARTBEAT_LOGIC_PERIOD_MS 1000U

static uint16_t hook_heartbeat_last_counter;
static uint32_t hook_heartbeat_last_counter_ms;
static bool hook_heartbeat_alarm_power_on;
/* Parked by the automatic 4G provisioning (module restarts mid-flow). */
static volatile bool hook_heartbeat_suspended;

/* Host heartbeat reception quality (see hook_heartbeat.h). */
static struct hook_link_stats hook_link;
static uint64_t hook_link_interval_sum_ms;
/* Wall clock of the most recent received heartbeat (0 = time not trusted). */
static int64_t hook_link_last_beat_unix_s;

void hook_heartbeat_get_link_stats(struct hook_link_stats *out)
{
	if (out == NULL) {
		return;
	}

	*out = hook_link;
	out->interval_avg_ms = (hook_link.received > 1U) ?
				       (uint32_t)(hook_link_interval_sum_ms /
						  (hook_link.received - 1U)) :
				       0U;
}

void hook_heartbeat_clear_link_stats(void)
{
	memset(&hook_link, 0, sizeof(hook_link));
	hook_link_interval_sum_ms = 0U;
}

/*
 * One received heartbeat: update the interval statistics and, when the gap
 * since the previous heartbeat exceeded CONFIG_HOOK_LINK_TIMEOUT_MS, count one
 * timeout and write exactly ONE offline log line for that outage.
 *
 * The line is written when the heartbeat comes back, not while the link is
 * down, so a 40 s outage with a 20 s threshold produces one line (gap 40 s),
 * never two, and a permanently offline host does not flood the SD card: the
 * outage itself is visible in the health event ("host_heartbeat" offline) and
 * in the fact that "received" stops growing.
 *
 * Soft standby does not reach this function at all (the standby branch in
 * hook_heartbeat_logic() only keeps the timers fed), so a nightly standby
 * never counts as a 4G timeout.
 */
static void hook_link_stats_update(uint32_t gap_ms)
{
	int64_t previous_beat_unix_s = hook_link_last_beat_unix_s;
	int64_t now_unix_s = 0;

#if defined(CONFIG_TIME_MANAGER_SERVICE)
	{
		int64_t utc_ms = 0;

		if (time_manager_get_utc_ms(&utc_ms) == 0) {
			now_unix_s = utc_ms / MSEC_PER_SEC;
		}
	}
#endif

	/*
	 * The first heartbeat after boot is not a heartbeat interval: its "gap"
	 * is the time from start-up to the first contact with the host, so it
	 * must not count as a link timeout. Counting it produced a "longest gap"
	 * larger than the maximum interval.
	 */
	if (hook_link.received == 0U) {
		hook_link.received++;
		hook_link_last_beat_unix_s = now_unix_s;
		return;
	}

	if ((hook_link.interval_min_ms == 0U) ||
	    (gap_ms < hook_link.interval_min_ms)) {
		hook_link.interval_min_ms = gap_ms;
	}
	if (gap_ms > hook_link.interval_max_ms) {
		hook_link.interval_max_ms = gap_ms;
	}
	hook_link_interval_sum_ms += gap_ms;

	hook_link.received++;
	hook_link_last_beat_unix_s = now_unix_s;

	if (gap_ms <= (uint32_t)CONFIG_HOOK_LINK_TIMEOUT_MS) {
		return;
	}

	hook_link.timeouts++;
	if (gap_ms > hook_link.timeout_max_ms) {
		hook_link.timeout_max_ms = gap_ms;
	}
	hook_link.last_timeout_ms = k_uptime_get_32();
	hook_link.last_timeout_unix_s = now_unix_s;

	(void)offline_log_event(OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_WRN,
				"host heartbeat timeout #%u: no heartbeat for %u ms "
				"(threshold %u ms, previous beat unix %lld, "
				"recovered at uptime %u ms)",
				hook_link.timeouts, gap_ms,
				(uint32_t)CONFIG_HOOK_LINK_TIMEOUT_MS,
				(long long)previous_beat_unix_s,
				hook_link.last_timeout_ms);
}

bool hook_heartbeat_alarm_power_is_on(void)
{
	return hook_heartbeat_alarm_power_on;
}

void hook_heartbeat_set_suspend(bool suspended)
{
	hook_heartbeat_suspended = suspended;

	/* Feed the timers across the suspension so the resume does not look
	 * like a 60 s silence.
	 */
	hook_heartbeat_last_counter_ms = hook_get_time_ms();
	hook_heartbeat_last_counter = hook_reg_get(CMD_HOST_HEARTBEAT);

	if (!suspended) {
		/*
		 * Keep the health service in step with the supervision: its
		 * "host_heartbeat offline" decision is based on the age of the
		 * last hook_health_report(), and without this it would fire up to
		 * a minute away from the 60 s supply cut (observed: offline at
		 * 65 s, alarm supply cut 37 s later).
		 */
		hook_health_report(HOOK_HEALTH_EVENT_HOST_HEARTBEAT);
	}
}

static void hook_heartbeat_set_alarm_power(bool on)
{
	GPIO_WRITE(alarm_power_en_gpio, on);
	hook_heartbeat_alarm_power_on = on;
}

void hook_heartbeat_init(void)
{
	hook_heartbeat_set_alarm_power(true);
	GPIO_WRITE(pwr_led_gpio, 0);
	hook_heartbeat_last_counter = hook_reg_get(CMD_HOST_HEARTBEAT);
	hook_heartbeat_last_counter_ms = hook_get_time_ms();
}

static void hook_heartbeat_logic(void)
{
	uint16_t current_counter;
	uint32_t now = hook_get_time_ms();
	uint32_t silence_ms;

	if (hook_reg_get(HEARTBEAT_ENABLE) == 0U) {
		/* Supervision disabled: keep the supply powered and keep feeding
		 * the internal timers so re-enabling does not trip instantly.
		 * Link quality statistics are not updated here either: without
		 * supervision there is no "expected" heartbeat cadence.
		 */
		if (!hook_heartbeat_alarm_power_on) {
			hook_heartbeat_set_alarm_power(true);
			LOG_INF("heartbeat supervision disabled, alarm power on");
		}
#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
		/* Supervision off also lifts a supply cut caused by an earlier
		 * timeout, so the alarm module is polled again.
		 */
		hook_alarm_set_suspend(false);
		hook_health_alarm_supply_restored();
#endif
		hook_heartbeat_last_counter_ms = now;
		hook_heartbeat_last_counter = hook_reg_get(CMD_HOST_HEARTBEAT);
		return;
	}

	if (is_soft_standby == 1U) {
		/* Soft standby: the host normally stops sending heartbeats while
		 * the controller sleeps, so this is not a communication problem.
		 * Only keep the timers fed - no statistics, no offline log, no
		 * timeout action. The first heartbeat after wake-up therefore
		 * shows a gap of at most one loop period, not the whole night.
		 */
		hook_heartbeat_last_counter_ms = now;
		hook_heartbeat_last_counter = hook_reg_get(CMD_HOST_HEARTBEAT);
		hook_heartbeat_alarm_power_on = false;
		return;
	}

	if (hook_heartbeat_suspended) {
		/* 4G provisioning: the module cannot deliver heartbeats while it
		 * saves and restarts. Keep the timers fed, take no action.
		 */
		hook_heartbeat_last_counter_ms = now;
		hook_heartbeat_last_counter = hook_reg_get(CMD_HOST_HEARTBEAT);
		return;
	}

	current_counter = hook_reg_get(CMD_HOST_HEARTBEAT);
	silence_ms = now - hook_heartbeat_last_counter_ms;

	if (current_counter != hook_heartbeat_last_counter) {
		uint32_t gap_ms = now - hook_heartbeat_last_counter_ms;

		hook_heartbeat_last_counter = current_counter;
		hook_heartbeat_last_counter_ms = now;
		hook_link_stats_update(gap_ms);

		if (!hook_heartbeat_alarm_power_on) {
			hook_heartbeat_set_alarm_power(true);
#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
			/* Supply is back: the module may be polled again and its
			 * offline detection leaves the "supply cut" state.
			 */
			hook_alarm_set_suspend(false);
			hook_alarm_force_volume_sync();
			hook_health_alarm_supply_restored();
#endif
			hook_reg_input_set(STATUS_LED_SWITCH, 0U);
			LOG_INF("host heartbeat back, alarm power on");
			(void)offline_log_event(
				OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_INF,
				"host heartbeat back after %u ms, alarm power on",
				gap_ms);
		}

		/* The health service owns the ERR_HEARTBEAT_TIMEOUT bit: this
		 * report keeps the event online, its callback clears the bit.
		 */
		hook_health_report(HOOK_HEALTH_EVENT_HOST_HEARTBEAT);
		return;
	}

	if (silence_ms > HEARTBEAT_SILENCE_MS) {
		hook_reg_set(CMD_LED_SWITCH, 0U);
		hook_reg_set(CMD_BUZZER_7M, 0U);
		hook_reg_set(CMD_BUZZER_3M, 0U);
	}

	if (silence_ms > HEARTBEAT_TIMEOUT_MS && hook_heartbeat_alarm_power_on) {
		hook_reg_set(CMD_LED_SWITCH, 0U);
		hook_reg_set(CMD_BUZZER_7M, 0U);
		hook_reg_set(CMD_BUZZER_3M, 0U);

#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
		/* Stop talking to the module first: it is about to lose power
		 * and can no longer answer, and report it offline right away
		 * with "host heartbeat timeout" as the reason. hook_health
		 * parks the 15 s offline detection until the heartbeat is
		 * back (see hook_health_alarm_supply_lost()).
		 */
		hook_alarm_set_suspend(true);
#endif
		hook_heartbeat_set_alarm_power(false);
		/* The CubeMX firmware (application/app_relay.c) cut PWD_LED as well.
		 * Drive it explicitly here so the indication is off immediately -
		 * and also when the sound & light module handling is compiled out.
		 * app/hook_alarm keeps mirroring CMD_LED_SWITCH (now 0), so the two
		 * writers agree.
		 */
		GPIO_WRITE(pwr_led_gpio, 0);

#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
		hook_health_alarm_supply_lost();
#endif

		(void)offline_log_event(
			OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_ERR,
			"host heartbeat lost for %u ms, alarm power off",
			(uint32_t)HEARTBEAT_TIMEOUT_MS);

		LOG_ERR("host heartbeat lost for %u ms, alarm power off",
			HEARTBEAT_TIMEOUT_MS);
	}
}

static void hook_heartbeat_thread(void *a, void *b, void *c)
{
	static uint32_t last_logic_ms;

	(void)a;
	(void)b;
	(void)c;

	for (;;) {
		/* Liveness check-in first: it must also happen while the
		 * supervision itself is idle (soft standby / host disabled).
		 */
		hook_health_task_alive(HOOK_TASK_ALIVE_HEARTBEAT);

		if (hook_get_time_ms() - last_logic_ms >=
		    HOOK_HEARTBEAT_LOGIC_PERIOD_MS) {
			last_logic_ms = hook_get_time_ms();
			hook_heartbeat_logic();
		}

		k_msleep(100);
	}
}

K_THREAD_STACK_DEFINE(hook_heartbeat_stack, 2048);
static struct k_thread hook_heartbeat_thread_obj;

void hook_heartbeat_start(void)
{
	k_thread_create(&hook_heartbeat_thread_obj, hook_heartbeat_stack,
			K_THREAD_STACK_SIZEOF(hook_heartbeat_stack),
			hook_heartbeat_thread, NULL, NULL, NULL,
			OSAL_PRIO_ABOVE_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_heartbeat_thread_obj, "HostHeartbeat");
}
