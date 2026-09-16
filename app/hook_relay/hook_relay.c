/*
 * hook_relay.c - host heartbeat supervision.
 *
 * The crane host controller increments holding register STATUS_HEART_BEAT
 * (104) periodically. This module mirrors that heartbeat onto the relay
 * output (FLASH_LIGHT_POWER_EN, PE5) and the relay state LED (PWD_LED, PB3):
 *
 *   heartbeat moving          -> relay on, ERR_HEARTBEAT_TIMEOUT cleared
 *   silence > 10 s            -> LED/buzzer commands cleared
 *   silence > 60 s            -> relay off + ERR_HEARTBEAT_TIMEOUT set
 *   HEARTBEAT_ENABLE (117)=0  -> supervision disabled, relay stays on
 *   soft standby              -> relay off, timers kept "fed"
 *
 * Only the relay output (FLASH_LIGHT_POWER_EN) is driven here. The relay
 * state LED PWD_LED is driven by app/hook_alarm (it mirrors the Modbus
 * flash-light command register) and by the soft standby / POST paths, which
 * is the behaviour of the latest CubeMX firmware.
 */
#include "hook_relay.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "board.h"
#include "hook_alarm.h"
#include "hook_health.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "osal_zephyr.h"
#include "system_def.h"

LOG_MODULE_REGISTER(hook_relay, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_RELAY_LOGIC_PERIOD_MS 1000U

static uint16_t hook_relay_last_heartbeat;
static uint32_t hook_relay_last_heartbeat_ms;
static bool hook_relay_on;

static void hook_relay_set_outputs(bool on)
{
	GPIO_WRITE(relay_en_gpio, on);
}

bool hook_relay_is_on(void)
{
	return hook_relay_on;
}

void hook_relay_init(void)
{
	hook_relay_set_outputs(true);
	GPIO_WRITE(pwr_led_gpio, 0);
	hook_relay_last_heartbeat = hook_reg_get(STATUS_HEART_BEAT);
	hook_relay_last_heartbeat_ms = hook_get_time_ms();
	hook_relay_on = true;
}

static void hook_relay_logic(void)
{
	uint16_t current_heartbeat;
	uint32_t now = hook_get_time_ms();
	uint32_t silence_ms;

	if (hook_reg_get(HEARTBEAT_ENABLE) == 0U) {
		/* Supervision disabled: keep the relay powered and keep feeding
		 * the internal timers so re-enabling does not trip instantly.
		 */
		if (!hook_relay_on) {
			hook_relay_set_outputs(true);
			hook_relay_on = true;
			LOG_INF("heartbeat supervision disabled, relay on");
		}
		hook_relay_last_heartbeat_ms = now;
		hook_relay_last_heartbeat = hook_reg_get(STATUS_HEART_BEAT);
		return;
	}

	if (is_soft_standby == 1U) {
		hook_relay_last_heartbeat_ms = now;
		hook_relay_last_heartbeat = hook_reg_get(STATUS_HEART_BEAT);
		hook_relay_on = false;
		return;
	}

	current_heartbeat = hook_reg_get(STATUS_HEART_BEAT);
	silence_ms = now - hook_relay_last_heartbeat_ms;

	if (current_heartbeat != hook_relay_last_heartbeat) {
		hook_relay_last_heartbeat = current_heartbeat;
		hook_relay_last_heartbeat_ms = now;

		if (!hook_relay_on) {
			hook_relay_set_outputs(true);
			hook_relay_on = true;
#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
			hook_alarm_force_volume_sync();
#endif
			hook_reg_set(STATUS_LED_SWITCH, 0U);
			LOG_INF("host heartbeat back, relay on");
		}

		if ((hook_reg_get_error_bits() & ERR_HEARTBEAT_TIMEOUT) != 0U) {
			hook_reg_clear_error_bits(ERR_HEARTBEAT_TIMEOUT);
		}

		hook_health_report(HOOK_HEALTH_EVENT_HOST_HEARTBEAT);
		return;
	}

	if (silence_ms > HEARTBEAT_SILENCE_MS) {
		hook_reg_set(CMD_LED_SWITCH, 0U);
		hook_reg_set(CMD_BUZZER_7M, 0U);
		hook_reg_set(CMD_BUZZER_3M, 0U);
	}

	if (silence_ms > HEARTBEAT_TIMEOUT_MS && hook_relay_on) {
		hook_reg_set(CMD_LED_SWITCH, 0U);
		hook_reg_set(CMD_BUZZER_7M, 0U);
		hook_reg_set(CMD_BUZZER_3M, 0U);

		hook_relay_on = false;
		hook_relay_set_outputs(false);
		hook_reg_set_error_bits(ERR_HEARTBEAT_TIMEOUT);

		LOG_ERR("host heartbeat lost for %u ms, relay off",
			HEARTBEAT_TIMEOUT_MS);
	}
}

static void hook_relay_thread(void *a, void *b, void *c)
{
	static uint32_t last_logic_ms;

	(void)a;
	(void)b;
	(void)c;

	for (;;) {
		hook_health_task_alive(HOOK_TASK_ALIVE_RELAY);

		if (hook_get_time_ms() - last_logic_ms >=
		    HOOK_RELAY_LOGIC_PERIOD_MS) {
			last_logic_ms = hook_get_time_ms();
			hook_relay_logic();
		}

		k_msleep(100);
	}
}

K_THREAD_STACK_DEFINE(hook_relay_stack, 2048);
static struct k_thread hook_relay_thread_obj;

void hook_relay_start(void)
{
	k_thread_create(&hook_relay_thread_obj, hook_relay_stack,
			K_THREAD_STACK_SIZEOF(hook_relay_stack),
			hook_relay_thread, NULL, NULL, NULL,
			OSAL_PRIO_ABOVE_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_relay_thread_obj, "RelayHeartbeat");
}
