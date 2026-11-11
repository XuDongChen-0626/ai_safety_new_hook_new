/*
 * hook_health.c - hook product health event table and check-in supervision.
 */
#include "hook_health.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>

#include "board.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "offline_log_service.h"
#include "system_def.h"
#include "system_health_service.h"
#if defined(CONFIG_TIME_MANAGER_SERVICE)
#include "time_manager_service.h"
#endif

LOG_MODULE_REGISTER(hook_health, CONFIG_LOG_DEFAULT_LEVEL);

/* Check-in supervision */
#define HOOK_HEALTH_BOOT_GRACE_MS 20000U
/*
 * One polling round of the slowest thread (BMS) can take ~6 s when every
 * Modbus query times out, so the window has to stay above that; the IWDG is
 * still stopped well before its 20 s timeout when a thread really hangs.
 */
#define HOOK_HEALTH_TASK_TIMEOUT_MS 12000U
#define HOOK_HEALTH_CHECK_PERIOD_MS 1000U

/*
 * Field device offline timeouts. These are the *only* online/offline criteria
 * of the product: the polling modules report every successful exchange
 * (hook_health_report) and do nothing else, the service decides when a device
 * is offline, and the event callbacks below perform every consequence
 * (error-code bits, logs, action policy) - the same split as
 * craner_ai_safety_car ("health callbacks are the single owner of the readable
 * /error state registers, polling threads only report successful reads").
 */
/*
 * The BMS answers every 500 ms (it keeps being polled in soft standby, so the
 * detection stays active there as well); 10 s = 20 missed exchanges. This is
 * the offline criterion for both the ERR_BMS_READ_FAIL bit and the health
 * event.
 */
#define HOOK_HEALTH_BMS_TIMEOUT_MS 10000U
/* The alarm module is polled at least every 3 s (hook_alarm_volume_sync); */
/* 15 s tolerates 5 consecutive misses. */
#define HOOK_HEALTH_ALARM_TIMEOUT_MS 15000U
/*
 * The host controller must move the heartbeat counter once per minute.
 * hook_heartbeat cuts the alarm supply at HEARTBEAT_TIMEOUT_MS (60 s) as a
 * product action; the offline decision (and the ERR_HEARTBEAT_TIMEOUT bit)
 * gets 5 s extra margin so a heartbeat that arrives exactly on the 60 s
 * boundary is not flagged.
 */
#define HOOK_HEALTH_HOST_TIMEOUT_MS (HEARTBEAT_TIMEOUT_MS + 5000U)
/* Offline-log category of each field event (the SD card keeps the history). */
static enum offline_log_category hook_health_log_category(uint16_t event)
{
	switch (event) {
	case HOOK_HEALTH_EVENT_BMS:
		return OFFLINE_LOG_CAT_BMS;
	case HOOK_HEALTH_EVENT_ALARM:
		return OFFLINE_LOG_CAT_ALARM;
	case HOOK_HEALTH_EVENT_HOST_HEARTBEAT:
		return OFFLINE_LOG_CAT_NETWORK;
	default:
		return OFFLINE_LOG_CAT_SYSTEM;
	}
}

/*
 * The two callbacks below are the single owner of the device error bits in the
 * error-code register: a device is offline exactly when the service says so
 * (offline_timeout_ms without a hook_health_report()), and the same callback
 * that logs it also raises the bit, so the error code, the work mode and the
 * health statistics can never disagree. The polling modules only report
 * successful exchanges.
 */
static void hook_health_offline_first(const struct sys_health_event_status *st)
{
	LOG_WRN("field device %s offline after %u ms", st->name,
		st->offline_timeout_ms);

	(void)offline_log_event(hook_health_log_category(st->event),
				OFFLINE_LOG_LEVEL_WRN,
				"field device %s offline (timeout %u ms, "
				"actions 0x%08x)",
				st->name, st->offline_timeout_ms, st->action_mask);

	switch (st->event) {
	case HOOK_HEALTH_EVENT_BMS:
		hook_reg_set_error_bits(ERR_BMS_READ_FAIL);
		break;
	case HOOK_HEALTH_EVENT_ALARM:
		hook_reg_set_error_bits(ERR_ALARM_OFFLINE);
		break;
	case HOOK_HEALTH_EVENT_HOST_HEARTBEAT:
		hook_reg_set_error_bits(ERR_HEARTBEAT_TIMEOUT);
		break;
	default:
		break;
	}
}

static void hook_health_online_first(const struct sys_health_event_status *st)
{
	LOG_INF("field device %s online again", st->name);

	(void)offline_log_event(hook_health_log_category(st->event),
				OFFLINE_LOG_LEVEL_INF,
				"field device %s online again (offline for %u ms)",
				st->name, st->last_offline_duration_ms);

	switch (st->event) {
	case HOOK_HEALTH_EVENT_BMS:
		hook_reg_clear_error_bits(ERR_BMS_READ_FAIL);
		break;
	case HOOK_HEALTH_EVENT_ALARM:
		hook_reg_clear_error_bits(ERR_ALARM_OFFLINE);
		break;
	case HOOK_HEALTH_EVENT_HOST_HEARTBEAT:
		hook_reg_clear_error_bits(ERR_HEARTBEAT_TIMEOUT);
		break;
	default:
		break;
	}
}

static const struct sys_health_event hook_health_events[] = {
	{
		.event = HOOK_HEALTH_EVENT_TASK_ALIVE,
		.name = "task_alive",
		.enable = true,
		/*
		 * Display-only priority (the LED backend blinks this many times
		 * and shows the smallest number when several events are
		 * offline). A task that stops checking in is rebooted by the
		 * watchdog, it is not a field-device fault, so it goes last.
		 */
		.priority = 4U,
		.offline_timeout_ms = HOOK_HEALTH_TASK_TIMEOUT_MS,
		.action_mask = SYS_HEALTH_ACTION_LOG |
			       SYS_HEALTH_ACTION_SET_DEGRADED |
			       SYS_HEALTH_ACTION_STOP_WATCHDOG_FEED,
		.action_delay_ms = 0U,
	},
	{
		.event = HOOK_HEALTH_EVENT_BMS,
		.name = "bms",
		.enable = IS_ENABLED(CONFIG_HOOK_BMS_ENABLE),
		.priority = 3U,
		.offline_timeout_ms = HOOK_HEALTH_BMS_TIMEOUT_MS,
		.action_mask = SYS_HEALTH_ACTION_LOG,
		.offline_first_func = hook_health_offline_first,
		.online_first_func = hook_health_online_first,
	},
	{
		.event = HOOK_HEALTH_EVENT_ALARM,
		.name = "alarm_module",
		.enable = IS_ENABLED(CONFIG_HOOK_ALARM_MODULE_ENABLE),
		/* 2 blinks, as the old hook_supervisor LED showed. */
		.priority = 2U,
		.offline_timeout_ms = HOOK_HEALTH_ALARM_TIMEOUT_MS,
		.action_mask = SYS_HEALTH_ACTION_LOG,
		.offline_first_func = hook_health_offline_first,
		.online_first_func = hook_health_online_first,
	},
	{
		.event = HOOK_HEALTH_EVENT_HOST_HEARTBEAT,
		.name = "host_heartbeat",
		.enable = IS_ENABLED(CONFIG_HOOK_HOST_HEARTBEAT_ENABLE),
		/* 1 blink: the root cause that also cuts the alarm supply. */
		.priority = 1U,
		.offline_timeout_ms = HOOK_HEALTH_HOST_TIMEOUT_MS,
		.action_mask = SYS_HEALTH_ACTION_LOG,
		.offline_first_func = hook_health_offline_first,
		.online_first_func = hook_health_online_first,
	},
};

/*
 * Events that are paused while the controller is in soft standby.
 *
 * The BMS is deliberately NOT in this list: the CubeMX firmware kept polling
 * the BMS during soft standby (modbus_bms_handle() had no standby check) and
 * this port keeps doing so, so its offline detection must stay active too.
 * The alarm module stops talking to the sound & light module in standby
 * (modbus_alarm_handle() returned early there) and the host heartbeat
 * supervision keeps feeding its own timers, so those two only produce noise
 * while asleep and are disabled.
 */
static const uint16_t hook_health_standby_paused_events[] = {
	HOOK_HEALTH_EVENT_ALARM,
	HOOK_HEALTH_EVENT_HOST_HEARTBEAT,
};

static atomic_t hook_health_alive_mask;
static bool hook_health_ota_active;
/* True while the alarm supply is cut because of a host heartbeat timeout.
 * Written by app/hook_heartbeat, read by app/hook_rtc (soft standby).
 */
static volatile bool hook_health_alarm_supply_cut;

void hook_health_task_alive(uint32_t mask)
{
	atomic_or(&hook_health_alive_mask, (atomic_val_t)mask);
}

void hook_health_report(uint16_t event)
{
	sys_health_event_report(event);
}

void hook_health_sleep_update(bool sleeping)
{
	for (size_t i = 0U; i < ARRAY_SIZE(hook_health_standby_paused_events); i++) {
		uint16_t event = hook_health_standby_paused_events[i];

		if (sleeping) {
			sys_health_event_disable(event);
		} else {
			/*
			 * A heartbeat timeout before the standby left the
			 * alarm detection parked: its supply is still cut, so
			 * keep it parked until the host heartbeat comes back
			 * (hook_health_alarm_supply_restored()).
			 */
			if ((event == HOOK_HEALTH_EVENT_ALARM) &&
			    hook_health_alarm_supply_cut) {
				continue;
			}
			sys_health_event_enable(event);
			sys_health_event_report(event);
		}
	}

	LOG_INF("alarm/heartbeat health events %s (BMS supervision stays active)",
		sleeping ? "disabled for soft standby" : "enabled");
}

void hook_health_alarm_supply_lost(void)
{
	if (hook_health_alarm_supply_cut) {
		return;
	}

	hook_health_alarm_supply_cut = true;

	/*
	 * Report the module offline with the real reason and park its offline
	 * detection in the same step: with the supply cut the module can never
	 * answer, so the regular 15 s "no answer" timeout would only repeat
	 * the same conclusion later, with a misleading cause.
	 */
	hook_reg_set_error_bits(ERR_ALARM_OFFLINE);
	sys_health_event_disable(HOOK_HEALTH_EVENT_ALARM);

	LOG_WRN("alarm module offline: host heartbeat timeout, supply cut");
	(void)offline_log_event(
		OFFLINE_LOG_CAT_ALARM, OFFLINE_LOG_LEVEL_WRN,
		"alarm module offline: host heartbeat timeout (%u ms), supply cut",
		(uint32_t)HEARTBEAT_TIMEOUT_MS);
}

void hook_health_alarm_supply_restored(void)
{
	if (!hook_health_alarm_supply_cut) {
		return;
	}

	hook_health_alarm_supply_cut = false;

	/* sys_health_event_enable() restarts the 15 s window from now, so the
	 * first exchange with the re-powered module is expected immediately.
	 */
	sys_health_event_enable(HOOK_HEALTH_EVENT_ALARM);
	hook_reg_clear_error_bits(ERR_ALARM_OFFLINE);

	LOG_INF("alarm module supervision resumed (supply restored)");
	(void)offline_log_event(
		OFFLINE_LOG_CAT_ALARM, OFFLINE_LOG_LEVEL_INF,
		"alarm module supervision resumed, supply restored");
}

void hook_health_ota_gate(bool active)
{
	if (active == hook_health_ota_active) {
		return;
	}

	hook_health_ota_active = active;

	if (active) {
		sys_health_event_disable(HOOK_HEALTH_EVENT_TASK_ALIVE);
		sys_health_event_disable(HOOK_HEALTH_EVENT_HOST_HEARTBEAT);
		LOG_INF("task check-in and host heartbeat supervision suspended for OTA");
	} else {
		atomic_clear(&hook_health_alive_mask);
		sys_health_event_enable(HOOK_HEALTH_EVENT_TASK_ALIVE);
		sys_health_event_report(HOOK_HEALTH_EVENT_TASK_ALIVE);
		sys_health_event_enable(HOOK_HEALTH_EVENT_HOST_HEARTBEAT);
		sys_health_event_report(HOOK_HEALTH_EVENT_HOST_HEARTBEAT);
		LOG_INF("task check-in and host heartbeat supervision resumed");
	}
}

#if defined(CONFIG_TIME_MANAGER_SERVICE)
static int hook_health_time_get_unix_s(int64_t *unix_time_s, void *user_data)
{
	int64_t utc_ms;
	int ret;

	ARG_UNUSED(user_data);

	if (unix_time_s == NULL) {
		return -EINVAL;
	}

	ret = time_manager_get_utc_ms(&utc_ms);
	if (ret != 0) {
		return ret;
	}

	*unix_time_s = utc_ms / MSEC_PER_SEC;

	return 0;
}
#endif

static int hook_health_init(void)
{
	const struct sys_health_time_provider time_provider = {
#if defined(CONFIG_TIME_MANAGER_SERVICE)
		.get_unix_time_s = hook_health_time_get_unix_s,
#else
		.get_unix_time_s = NULL,
#endif
	};
	int ret;

	atomic_clear(&hook_health_alive_mask);

	ret = sys_health_init(hook_health_events,
			      (int)ARRAY_SIZE(hook_health_events),
			      &time_provider);
	if (ret != 0) {
		LOG_ERR("system health init failed: %d", ret);
		return ret;
	}

	LOG_INF("hook health ready (task check-in guard=0x%08X)",
		HOOK_TASK_ALIVE_ALL);

	return 0;
}

SYS_INIT(hook_health_init, APPLICATION, 80);

static const char *hook_health_missing_tasks(uint32_t missing)
{
	static char text[64];
	size_t used = 0U;

	text[0] = '\0';

	if ((missing & HOOK_TASK_ALIVE_ALARM) != 0U) {
		used += snprintk(&text[used], sizeof(text) - used, "alarm ");
	}
	if ((missing & HOOK_TASK_ALIVE_BMS) != 0U) {
		used += snprintk(&text[used], sizeof(text) - used, "bms ");
	}
	if ((missing & HOOK_TASK_ALIVE_HEARTBEAT) != 0U) {
		used += snprintk(&text[used], sizeof(text) - used, "heartbeat ");
	}
	if ((missing & HOOK_TASK_ALIVE_RTC) != 0U) {
		used += snprintk(&text[used], sizeof(text) - used, "rtc ");
	}
	if ((missing & HOOK_TASK_ALIVE_4G) != 0U) {
		(void)snprintk(&text[used], sizeof(text) - used, "4g ");
	}

	return text;
}

static void hook_health_thread(void *a, void *b, void *c)
{
	uint32_t started_ms;
	uint8_t miss_count = 0U;

	(void)a;
	(void)b;
	(void)c;

	started_ms = hook_get_time_ms();

	for (;;) {
		uint32_t mask = (uint32_t)atomic_set(&hook_health_alive_mask, 0);
		uint32_t missing = HOOK_TASK_ALIVE_ALL & ~mask;

		if (hook_health_ota_active) {
			/* OTA in progress: the check-in window does not apply. */
			k_msleep(HOOK_HEALTH_CHECK_PERIOD_MS);
			continue;
		}

		if (hook_get_time_ms() - started_ms < HOOK_HEALTH_BOOT_GRACE_MS) {
			/*
			 * Supervision has not started yet (the slowest thread is
			 * the alarm module: its power-on self test blocks for
			 * ~8 s before its first check-in). Keep the service event
			 * ONLINE while the grace runs: the service judges
			 * "offline" purely from the age of the last
			 * hook_health_report(), and its timeout for this event
			 * (HOOK_HEALTH_TASK_TIMEOUT_MS = 12 s) is SHORTER than
			 * this grace. Staying silent here latched the
			 * STOP_WATCHDOG_FEED action at every boot ("System
			 * protect latched by event=1 task_alive" 12 s after
			 * reset) and, with the watchdog feed backend enabled,
			 * would have reset the controller in a loop.
			 */
			hook_health_report(HOOK_HEALTH_EVENT_TASK_ALIVE);
			k_msleep(HOOK_HEALTH_CHECK_PERIOD_MS);
			continue;
		}

		if (missing == 0U) {
			if (miss_count >= 6U) {
				LOG_INF("all application tasks checked in again");
			}
			miss_count = 0U;
			hook_health_report(HOOK_HEALTH_EVENT_TASK_ALIVE);
		} else {
			if (miss_count < 6U) {
				miss_count++;
			}

			LOG_WRN_RATELIMIT("task check-in delayed (%u s): %s",
					  miss_count,
					  hook_health_missing_tasks(missing));
		}

		k_msleep(HOOK_HEALTH_CHECK_PERIOD_MS);
	}
}

K_THREAD_STACK_DEFINE(hook_health_stack, 2048);
static struct k_thread hook_health_thread_obj;

void hook_health_task_start(void)
{
	k_thread_create(&hook_health_thread_obj, hook_health_stack,
			K_THREAD_STACK_SIZEOF(hook_health_stack),
			hook_health_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	k_thread_name_set(&hook_health_thread_obj, "HookHealth");
}
