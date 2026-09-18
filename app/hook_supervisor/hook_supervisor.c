/*
 * hook_supervisor.c - heart LED animation thread and work mode aggregation.
 *
 * Two threads behave exactly like the two CubeMX tasks:
 *   SysSupervisor (200 ms): render the error code on the heart LED
 *   WorkModeTask  (100 ms): aggregate the work mode into register 106
 */
#include "hook_supervisor.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "board.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "osal_zephyr.h"
#include "system_def.h"

LOG_MODULE_REGISTER(hook_supervisor, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_LED_STEP_PERIOD_MS 200U
#define HOOK_LED_PAUSE_MS 2000U

void hook_led_animation_step(hook_led_anim_t *ctx, hook_led_mode_t mode,
			     uint8_t blink_count)
{
	uint16_t steps_1s;

	if (ctx->step_period_ms == 0U) {
		ctx->step_period_ms = HOOK_LED_STEP_PERIOD_MS;
	}

	if (ctx->last_mode != mode || ctx->last_count != blink_count) {
		ctx->cycle_step = 0U;
		ctx->last_mode = mode;
		ctx->last_count = blink_count;
	}

	switch (mode) {
	case HOOK_LED_MODE_NORMAL:
		steps_1s = (uint16_t)(1000U / ctx->step_period_ms);
		GPIO_WRITE(*ctx->led, (ctx->cycle_step < steps_1s));
		if (++ctx->cycle_step >= (uint16_t)(steps_1s * 2U)) {
			ctx->cycle_step = 0U;
		}
		break;

	case HOOK_LED_MODE_BURST:
		GPIO_WRITE(*ctx->led, ((ctx->cycle_step % 2U) == 0U));
		if (++ctx->cycle_step >= 2U) {
			ctx->cycle_step = 0U;
		}
		break;

	case HOOK_LED_MODE_BLINK_PAUSE:
		if (blink_count > 0U) {
			uint16_t blink_steps = (uint16_t)(blink_count * 2U);
			uint16_t pause_steps =
				(uint16_t)(HOOK_LED_PAUSE_MS / ctx->step_period_ms);
			uint16_t total_steps = (uint16_t)(blink_steps + pause_steps);

			if (ctx->cycle_step < blink_steps) {
				GPIO_WRITE(*ctx->led,
					   ((ctx->cycle_step % 2U) == 0U));
			} else {
				GPIO_WRITE(*ctx->led, false);
			}

			if (++ctx->cycle_step >= total_steps) {
				ctx->cycle_step = 0U;
			}
		} else {
			GPIO_WRITE(*ctx->led, false);
			ctx->cycle_step = 0U;
		}
		break;
	}
}

static hook_led_anim_t hook_heart_led = {
	.led = &heartbeat_led_gpio,
	.step_period_ms = HOOK_LED_STEP_PERIOD_MS,
	.cycle_step = 0U,
	.last_mode = HOOK_LED_MODE_NORMAL,
	.last_count = 0U,
};

static void hook_supervisor_process(void)
{
	uint16_t error = hook_reg_input_get(REG_ERROR_CODE);
	hook_led_mode_t mode = HOOK_LED_MODE_NORMAL;
	uint8_t blink_count = 0U;

	if (error != (uint16_t)ERR_NONE) {
		if ((error & (uint16_t)(error - 1U)) != 0U) {
			/* Multiple simultaneous errors: burst. */
			mode = HOOK_LED_MODE_BURST;
		} else {
			mode = HOOK_LED_MODE_BLINK_PAUSE;
			switch (error) {
			case ERR_HEARTBEAT_TIMEOUT:
				blink_count = 1U;
				break;
			case ERR_LED_OFFLINE:
				blink_count = 2U;
				break;
			case ERR_BMS_READ_FAIL:
				blink_count = 3U;
				break;
			default:
				blink_count = 1U;
				break;
			}
		}
	}

	hook_led_animation_step(&hook_heart_led, mode, blink_count);
}

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

static void hook_supervisor_thread(void *a, void *b, void *c)
{
	(void)a;
	(void)b;
	(void)c;

	for (;;) {
		hook_supervisor_process();
		k_msleep(200);
	}
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

K_THREAD_STACK_DEFINE(hook_supervisor_stack, 2048);
static struct k_thread hook_supervisor_thread_obj;

K_THREAD_STACK_DEFINE(hook_work_mode_stack, 2048);
static struct k_thread hook_work_mode_thread_obj;

void hook_supervisor_start(void)
{
	k_thread_create(&hook_supervisor_thread_obj, hook_supervisor_stack,
			K_THREAD_STACK_SIZEOF(hook_supervisor_stack),
			hook_supervisor_thread, NULL, NULL, NULL,
			OSAL_PRIO_BELOW_NORMAL1, 0, K_NO_WAIT);
	k_thread_name_set(&hook_supervisor_thread_obj, "SysSupervisor");

	k_thread_create(&hook_work_mode_thread_obj, hook_work_mode_stack,
			K_THREAD_STACK_SIZEOF(hook_work_mode_stack),
			hook_work_mode_thread, NULL, NULL, NULL,
			OSAL_PRIO_BELOW_NORMAL1, 0, K_NO_WAIT);
	k_thread_name_set(&hook_work_mode_thread_obj, "WorkModeTask");
}
