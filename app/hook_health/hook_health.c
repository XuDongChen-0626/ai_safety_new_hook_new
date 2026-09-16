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
#include "osal_zephyr.h"
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

/* Field device timeouts */
#define HOOK_HEALTH_BMS_TIMEOUT_MS 25000U
#define HOOK_HEALTH_ALARM_TIMEOUT_MS 25000U
#define HOOK_HEALTH_4G_TIMEOUT_MS 300000U
#define HOOK_HEALTH_GPS_TIMEOUT_MS 600000U
#define HOOK_HEALTH_HOST_TIMEOUT_MS (HEARTBEAT_TIMEOUT_MS + 5000U)

/*
 * Field events are for observability (logs + the health shell): the error
 * code register (113) stays owned by the module that detects the fault
 * (hook_bms / hook_alarm / hook_relay), exactly like the CubeMX firmware, so
 * every bit has a single writer.
 */
static void hook_health_offline_first(const struct sys_health_event_status *st)
{
	LOG_WRN("field event %s offline after %u ms", st->name,
		st->offline_timeout_ms);
}

static void hook_health_online_first(const struct sys_health_event_status *st)
{
	LOG_INF("field event %s online again", st->name);
}

static const struct sys_health_event hook_health_events[] = {
	{
		.event = HOOK_HEALTH_EVENT_TASK_ALIVE,
		.name = "task_alive",
		.enable = true,
		.priority = 1U,
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
		.priority = 3U,
		.offline_timeout_ms = HOOK_HEALTH_ALARM_TIMEOUT_MS,
		.action_mask = SYS_HEALTH_ACTION_LOG,
		.offline_first_func = hook_health_offline_first,
		.online_first_func = hook_health_online_first,
	},
	{
		.event = HOOK_HEALTH_EVENT_GPS,
		.name = "gps",
		.enable = IS_ENABLED(CONFIG_HOOK_GPS_ENABLE),
		.priority = 4U,
		.offline_timeout_ms = HOOK_HEALTH_GPS_TIMEOUT_MS,
		.action_mask = SYS_HEALTH_ACTION_LOG,
	},
	{
		.event = HOOK_HEALTH_EVENT_HOST_HEARTBEAT,
		.name = "host_heartbeat",
		.enable = IS_ENABLED(CONFIG_HOOK_HEARTBEAT_RELAY_ENABLE),
		.priority = 2U,
		.offline_timeout_ms = HOOK_HEALTH_HOST_TIMEOUT_MS,
		.action_mask = SYS_HEALTH_ACTION_LOG,
		.offline_first_func = hook_health_offline_first,
		.online_first_func = hook_health_online_first,
	},
	{
		.event = HOOK_HEALTH_EVENT_4G,
		.name = "modem_4g",
		.enable = IS_ENABLED(CONFIG_HOOK_4G_ENABLE),
		.priority = 4U,
		.offline_timeout_ms = HOOK_HEALTH_4G_TIMEOUT_MS,
		.action_mask = SYS_HEALTH_ACTION_LOG,
	},
};

static const uint16_t hook_health_field_events[] = {
	HOOK_HEALTH_EVENT_BMS,
	HOOK_HEALTH_EVENT_ALARM,
	HOOK_HEALTH_EVENT_GPS,
	HOOK_HEALTH_EVENT_HOST_HEARTBEAT,
	HOOK_HEALTH_EVENT_4G,
};

static atomic_t hook_health_alive_mask;
static bool hook_health_ota_active;

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
	for (size_t i = 0U; i < ARRAY_SIZE(hook_health_field_events); i++) {
		uint16_t event = hook_health_field_events[i];

		if (sleeping) {
			sys_health_event_disable(event);
		} else {
			sys_health_event_enable(event);
			sys_health_event_report(event);
		}
	}

	LOG_INF("field health events %s",
		sleeping ? "disabled (soft standby)" : "enabled");
}

void hook_health_ota_gate(bool active)
{
	if (active == hook_health_ota_active) {
		return;
	}

	hook_health_ota_active = active;

	if (active) {
		sys_health_event_disable(HOOK_HEALTH_EVENT_TASK_ALIVE);
		LOG_INF("task check-in supervision suspended for OTA");
	} else {
		atomic_clear(&hook_health_alive_mask);
		sys_health_event_enable(HOOK_HEALTH_EVENT_TASK_ALIVE);
		sys_health_event_report(HOOK_HEALTH_EVENT_TASK_ALIVE);
		LOG_INF("task check-in supervision resumed");
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
	if ((missing & HOOK_TASK_ALIVE_RELAY) != 0U) {
		used += snprintk(&text[used], sizeof(text) - used, "relay ");
	}
	if ((missing & HOOK_TASK_ALIVE_GPS) != 0U) {
		used += snprintk(&text[used], sizeof(text) - used, "gps ");
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
			OSAL_PRIO_LOW, 0, K_NO_WAIT);
	k_thread_name_set(&hook_health_thread_obj, "HookHealth");
}
