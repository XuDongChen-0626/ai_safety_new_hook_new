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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "board.h"
#include "hook_alarm.h"
#include "hook_health.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "osal_zephyr.h"
#include "system_def.h"

LOG_MODULE_REGISTER(hook_heartbeat, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_HEARTBEAT_LOGIC_PERIOD_MS 1000U

static uint16_t hook_heartbeat_last_counter;
static uint32_t hook_heartbeat_last_counter_ms;
static bool hook_heartbeat_alarm_power_on;

bool hook_heartbeat_alarm_power_is_on(void)
{
	return hook_heartbeat_alarm_power_on;
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
		 */
		if (!hook_heartbeat_alarm_power_on) {
			hook_heartbeat_set_alarm_power(true);
			LOG_INF("heartbeat supervision disabled, alarm power on");
		}
		hook_heartbeat_last_counter_ms = now;
		hook_heartbeat_last_counter = hook_reg_get(CMD_HOST_HEARTBEAT);
		return;
	}

	if (is_soft_standby == 1U) {
		hook_heartbeat_last_counter_ms = now;
		hook_heartbeat_last_counter = hook_reg_get(CMD_HOST_HEARTBEAT);
		hook_heartbeat_alarm_power_on = false;
		return;
	}

	current_counter = hook_reg_get(CMD_HOST_HEARTBEAT);
	silence_ms = now - hook_heartbeat_last_counter_ms;

	if (current_counter != hook_heartbeat_last_counter) {
		hook_heartbeat_last_counter = current_counter;
		hook_heartbeat_last_counter_ms = now;

		if (!hook_heartbeat_alarm_power_on) {
			hook_heartbeat_set_alarm_power(true);
#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
			hook_alarm_force_volume_sync();
#endif
			hook_reg_input_set(STATUS_LED_SWITCH, 0U);
			LOG_INF("host heartbeat back, alarm power on");
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

		hook_heartbeat_set_alarm_power(false);
		/* The CubeMX firmware (application/app_relay.c) cut PWD_LED as well.
		 * Drive it explicitly here so the indication is off immediately -
		 * and also when the sound & light module handling is compiled out.
		 * app/hook_alarm keeps mirroring CMD_LED_SWITCH (now 0), so the two
		 * writers agree.
		 */
		GPIO_WRITE(pwr_led_gpio, 0);

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
