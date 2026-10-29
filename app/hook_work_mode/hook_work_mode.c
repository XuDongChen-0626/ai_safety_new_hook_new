/*
 * hook_work_mode.c - aggregate the device work mode into the input registers.
 *
 * One thread, ported from the old app_sys_supervisor.c WorkModeTask (100 ms):
 * the error code, the battery level and the soft-standby flag are combined into
 * STATUS_WORK_MODE (input register 106), hook_main_status and
 * is_power_sleep_flag.
 *
 * The heart LED is NOT handled here any more: system_health_service drives it
 * from the health events (CONFIG_SYS_HEALTH_LED_INDICATOR_BACKEND).
 */
#include "hook_work_mode.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "hook_register.h"
#include "hook_register_map.h"
#include "osal_zephyr.h"
#include "system_def.h"

LOG_MODULE_REGISTER(hook_work_mode, CONFIG_LOG_DEFAULT_LEVEL);

static void hook_work_mode_logic(void)
{
	uint16_t error = hook_reg_input_get(REG_ERROR_CODE);
	uint16_t battery = hook_reg_input_get(STATUS_BMS_BATTERY);
	hook_work_mode_t mode;

	is_power_sleep_flag = (is_soft_standby == 1U) ? 1U : 0U;

	if (error != (uint16_t)ERR_NONE) {
		mode = DEVICE_ERROR;
	} else if (is_power_sleep_flag == 1U) {
		mode = MODE_DEVICE_STANDBY;
	} else if (battery <= (uint16_t)LOW_BATTERY_THRESHOLD) {
		mode = MODE_LOW_BATTERY;
	} else {
		mode = MODE_COM_WORKING;
	}

	hook_main_status = (mode == MODE_DEVICE_STANDBY) ? 2U :
			   (mode == DEVICE_ERROR) ? 3U : 1U;
	hook_reg_input_set(STATUS_WORK_MODE, (uint16_t)mode);
}

static void hook_work_mode_thread(void *a, void *b, void *c)
{
	(void)a;
	(void)b;
	(void)c;

	for (;;) {
		hook_work_mode_logic();
		k_msleep(100);
	}
}

K_THREAD_STACK_DEFINE(hook_work_mode_stack, 2048);
static struct k_thread hook_work_mode_thread_obj;

void hook_work_mode_start(void)
{
	k_thread_create(&hook_work_mode_thread_obj, hook_work_mode_stack,
			K_THREAD_STACK_SIZEOF(hook_work_mode_stack),
			hook_work_mode_thread, NULL, NULL, NULL,
			OSAL_PRIO_BELOW_NORMAL1, 0, K_NO_WAIT);
	k_thread_name_set(&hook_work_mode_thread_obj, "WorkModeTask");
}