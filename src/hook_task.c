/*
 * hook_task.c - application thread creation.
 *
 * Replaces init_app_hook_task() of application/app_hook_task.c and the
 * FreeRTOS/CMSIS-OS task objects of the CubeMX firmware.
 */
#include "hook_task.h"

#include "board.h"
#include "hook_4g.h"
#include "hook_alarm.h"
#include "hook_bms.h"
#include "hook_health.h"
#include "hook_modbus.h"
#include "hook_ota.h"
#include "hook_heartbeat.h"
#include "hook_rtc.h"
#include "hook_supervisor.h"
#include "osal_zephyr.h"
#include "system_def.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hook_task, CONFIG_LOG_DEFAULT_LEVEL);

/* Shared runtime flags (old sys_def.c / GPS app globals). */
volatile uint8_t is_power_sleep_flag;
volatile uint8_t is_ota_updating;
volatile uint16_t is_soft_standby;
uint16_t hook_main_status = 1U;

void init_app_hook_task(void)
{
	LOG_INF("[HOOK] starting application");

	/* Register map defaults, host RTU slave and the two RTU masters. */
	hook_modbus_init();

	/* Business threads (each one is optional through the product config). */
#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
	hook_alarm_start();
#endif
#if defined(CONFIG_HOOK_BMS_ENABLE)
	hook_bms_start();
#endif
#if defined(CONFIG_HOOK_HOST_HEARTBEAT_ENABLE)
	hook_heartbeat_init();
	hook_heartbeat_start();
#endif
	hook_rtc_start();
#if defined(CONFIG_HOOK_4G_ENABLE)
	hook_4g_init();
	hook_4g_start();
#endif
#if defined(CONFIG_HOOK_OTA_ENABLE)
	hook_ota_init();
	hook_ota_start();
#endif
	hook_supervisor_start();
	hook_health_task_start();

	LOG_INF("[HOOK] application threads started");
}
