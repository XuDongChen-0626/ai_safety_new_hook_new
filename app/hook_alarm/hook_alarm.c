/*
 * hook_alarm.c - sound & light alarm module handling.
 *
 * The module is a Modbus RTU slave (address 1) on uart7:
 *   - 0x0003 sound/light trigger  (0x0008 = 7 m siren, 0x0009 = 3 m siren)
 *   - 0x0006 volume               (0..31)
 *   - 0x00C2 LED control          (0x0051 slow flash, 0x0060 off)
 *   - 0x000E stop sound
 */
#include "hook_alarm.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "board.h"
#include "hook_health.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "modbus_rtu_master.h"
#include "osal_zephyr.h"
#include "system_def.h"

LOG_MODULE_REGISTER(hook_alarm, CONFIG_LOG_DEFAULT_LEVEL);

/* Single voice playback lasts about 5.3 s on the module. */
#define HOOK_ALARM_VOICE_PLAY_MS 5300U

/* Volume polling period of the outer loop. */
#define HOOK_ALARM_LOGIC_PERIOD_MS 500U

/*
 * When the module does not answer, every command costs ~1 s (Modbus wait
 * timeout). Repeating the *same failed* value is limited to one attempt per
 * second so a missing module cannot flood the RS485 bus or starve the blink
 * engine. A *new* value is always sent immediately, see
 * hook_alarm_volume_sync().
 */
#define HOOK_ALARM_RETRY_INTERVAL_MS 1000U

/*
 * Liveness poll period used while the volume register does not change and the
 * flash light is idle: without it a quiet system would never talk to the
 * module and the offline detection would raise a false alarm.
 */
#define HOOK_ALARM_LIVENESS_PERIOD_MS 5000U

/*
 * The alarm module no longer drives its own slow-flash pattern: the
 * controller generates the 2 s on / 1 s off blink by writing CMD_LED_ON /
 * CMD_LED_OFF every 250 ms (same parameters as the latest CubeMX firmware).
 */
#define HOOK_ALARM_LED_STEP_MS   250U
#define HOOK_ALARM_LED_ON_STEPS  8U
#define HOOK_ALARM_LED_OFF_STEPS 4U
#define HOOK_ALARM_LED_CYCLE \
	(HOOK_ALARM_LED_ON_STEPS + HOOK_ALARM_LED_OFF_STEPS)

static osal_notify_t hook_alarm_notify;
static uint16_t hook_alarm_payload;

static modbus_t hook_alarm_telegram = {
	.u8id = SLAVE_LED_ID,
	.u8fct = MB_FC_WRITE_REGISTER,
	.u16CoilsNo = 1U,
	.u16reg = &hook_alarm_payload,
};

static uint16_t hook_alarm_volume_now;
/* Last volume value the module confirmed (0xFFFF = nothing confirmed yet). */
static uint16_t hook_alarm_volume_sent;
/* Last volume value we tried to send, confirmed or not. */
static uint16_t hook_alarm_volume_attempt;
static bool hook_alarm_led_online;
static uint32_t hook_alarm_last_led_retry_ms;

static uint32_t hook_alarm_write(uint16_t reg_addr, uint16_t value)
{
	hook_alarm_telegram.u16RegAdd = reg_addr;
	hook_alarm_payload = value;

	ModbusQuery(&hook_alarm_master, hook_alarm_telegram);

	return osal_notify_wait(&hook_alarm_notify, MODBUS_WAIT_TIMEOUT_MS);
}

void hook_alarm_force_volume_sync(void)
{
	/* Forget both the confirmed and the attempted value, so the next sync
	 * sends the current register value immediately.
	 */
	hook_alarm_volume_sent = 0xFFFFU;
	hook_alarm_volume_attempt = 0xFFFFU;
}

bool hook_alarm_led_is_online(void)
{
	return hook_alarm_led_online;
}

/*
 * Any answered request proves that the module is alive: report it to the
 * health service (app/hook_health), which owns the online/offline decision and
 * the ERR_LED_OFFLINE bit in its event callbacks. This module only reports
 * successes and logs the failures.
 */
static void hook_alarm_report_online(void)
{
	hook_alarm_led_online = true;
	hook_health_report(HOOK_HEALTH_EVENT_ALARM);
}

static void hook_alarm_log_failure(const char *what)
{
	hook_alarm_led_online = false;
	LOG_ERR_RATELIMIT("alarm module %s failed", what);
}

/*
 * Buzzer state machine: 3 m has priority over 7 m, and a command is only
 * re-issued after the previous voice finished playing.
 */
static void hook_alarm_buzzer_logic(void)
{
	typedef enum {
		BUZZER_STATE_IDLE = 0,
		BUZZER_STATE_BUSY,
	} buzzer_state_t;

	static buzzer_state_t state = BUZZER_STATE_IDLE;
	static uint32_t busy_start_ms;
	uint16_t raw_target_mode = 0U;
	uint16_t current_mode;
	uint32_t ret;

	if (hook_reg_get(CMD_BUZZER_3M) == 1U) {
		raw_target_mode = 2U;
	} else if (hook_reg_get(CMD_BUZZER_7M) == 1U) {
		raw_target_mode = 1U;
	}

	if (state == BUZZER_STATE_BUSY) {
		if (hook_get_time_ms() - busy_start_ms >= HOOK_ALARM_VOICE_PLAY_MS) {
			state = BUZZER_STATE_IDLE;
		} else {
			return;
		}
	}

	current_mode = hook_reg_input_get(STATUS_BUZZER);

	if (raw_target_mode == 0U && current_mode == 0U) {
		return;
	}

	if (raw_target_mode == 2U) {
		ret = hook_alarm_write(REG_SOUND_LIGHT_CTRL, CMD_SOUND_3M);
		if (ret != OP_OK_QUERY) {
			LOG_ERR("3 m siren write failed");
			return;
		}
		LOG_INF("3 m siren triggered");
		hook_reg_input_set(STATUS_BUZZER, 2U);
		state = BUZZER_STATE_BUSY;
		busy_start_ms = hook_get_time_ms();
	} else if (raw_target_mode == 1U) {
		ret = hook_alarm_write(REG_SOUND_LIGHT_CTRL, CMD_SOUND_7M);
		if (ret != OP_OK_QUERY) {
			LOG_ERR("7 m siren write failed");
			return;
		}
		LOG_INF("7 m siren triggered");
		hook_reg_input_set(STATUS_BUZZER, 1U);
		state = BUZZER_STATE_BUSY;
		busy_start_ms = hook_get_time_ms();
	} else {
		ret = hook_alarm_write(REG_SOUND_STOP, CMD_SOUND_STOP);
		if (ret != OP_OK_QUERY) {
			LOG_ERR("sound stop write failed");
			return;
		}
		LOG_INF("sound stopped");
		hook_reg_input_set(STATUS_BUZZER, 0U);
	}
}

static void hook_alarm_led_logic(void)
{
	static uint32_t last_step_ms;
	static uint32_t led_step_counter;
	static uint8_t current_physical_state = 0xFFU;
	uint16_t cmd_led_switch = hook_reg_get(CMD_LED_SWITCH);
	uint16_t status_buzzer = hook_reg_input_get(STATUS_BUZZER);
	uint16_t error_code = hook_reg_get_error_bits();
	uint32_t ret;

	/* PWD_LED mirrors the Modbus flash-light command register, not the
	 * supply state and not every blink phase.
	 */
	GPIO_WRITE(pwr_led_gpio, (cmd_led_switch == 1U));

	if ((cmd_led_switch == 1U) || (status_buzzer != 0U)) {
		/* Blink while the host requests the flash light or a siren runs. */
		if ((hook_get_time_ms() - last_step_ms) >= HOOK_ALARM_LED_STEP_MS) {
			uint8_t position = (uint8_t)(led_step_counter %
						     HOOK_ALARM_LED_CYCLE);
			uint8_t desired_state =
				(position < HOOK_ALARM_LED_ON_STEPS) ? 1U : 0U;

			last_step_ms = hook_get_time_ms();
			led_step_counter++;

			if ((desired_state != current_physical_state) ||
			    ((error_code & ERR_LED_OFFLINE) != 0U)) {
				bool offline_retry =
					(desired_state == current_physical_state);

				/* Offline re-probe at most once per second. */
				if (offline_retry &&
				    ((hook_get_time_ms() -
				      hook_alarm_last_led_retry_ms) <
				     HOOK_ALARM_RETRY_INTERVAL_MS)) {
					return;
				}
				hook_alarm_last_led_retry_ms = hook_get_time_ms();

				ret = hook_alarm_write(REG_LED_CTRL,
						       (desired_state != 0U) ?
							       CMD_LED_ON :
							       CMD_LED_OFF);
				if (ret == OP_OK_QUERY) {
					current_physical_state = desired_state;
					hook_reg_input_set(STATUS_LED_SWITCH, 1U);
					hook_alarm_report_online();
				} else {
					hook_alarm_log_failure(
						(desired_state != 0U) ? "LED on write" :
									"LED off write");
				}
			}
		}
	} else if ((current_physical_state != 0U) ||
		   ((error_code & ERR_LED_OFFLINE) != 0U)) {
		/* Flashing disabled: make sure the module LED is really off. */
		if ((hook_get_time_ms() - hook_alarm_last_led_retry_ms) <
		    HOOK_ALARM_RETRY_INTERVAL_MS) {
			return;
		}
		hook_alarm_last_led_retry_ms = hook_get_time_ms();

		ret = hook_alarm_write(REG_LED_CTRL, CMD_LED_OFF);
		if (ret == OP_OK_QUERY) {
			current_physical_state = 0U;
			led_step_counter = 0U;
			hook_reg_input_set(STATUS_LED_SWITCH, 0U);
			hook_alarm_report_online();
		} else {
			hook_alarm_log_failure("LED off write");
		}
	}
}

/*
 * Volume synchronisation and alarm module liveness.
 *
 * The volume register is the only exchange that is wanted even when nothing
 * is alarming, so it also carries the liveness check-in. Rules:
 *
 *   1. A new volume value is sent immediately - no rate limit on a value
 *      change, it may have to reach the module right away.
 *   2. Whatever happens, the controller talks to the module at least once
 *      every 3 s: after every exchange (value change or not) the keepalive
 *      timer restarts, so the maximum interval between two exchanges is 3 s.
 *      The keepalive is an idempotent write of the value already in use (a
 *      register read is not guaranteed to be supported by this module).
 *   3. Only repeating the *same failed* value is limited to one attempt per
 *      second: an unanswered Modbus attempt blocks this thread for ~1 s
 *      (MODBUS_WAIT_TIMEOUT_MS), so retrying faster would only starve the
 *      blink engine and flood the RS485 bus.
 *
 * A failed exchange is logged and (because it does not report success) lets
 * the health service's 15 s event timeout declare the module offline - so a
 * quiet system can no longer look offline just because nothing had to be sent.
 */
static void hook_alarm_volume_sync(void)
{
	static uint32_t last_attempt_ms;
	static uint32_t last_exchange_ms;
	uint32_t now = hook_get_time_ms();
	uint32_t ret;

	hook_alarm_volume_now = hook_reg_get(CMD_VOLUME);

	if (hook_alarm_volume_now != hook_alarm_volume_sent) {
		bool repeating_same_failed_value =
			(hook_alarm_volume_now == hook_alarm_volume_attempt);

		/* Rule 1/3: a new value goes out at once, only the retry of the
		 * same (still unconfirmed) value is slowed down.
		 */
		if (repeating_same_failed_value &&
		    ((now - last_attempt_ms) < HOOK_ALARM_RETRY_INTERVAL_MS)) {
			return;
		}

		last_attempt_ms = now;
		last_exchange_ms = now;
		hook_alarm_volume_attempt = hook_alarm_volume_now;

		ret = hook_alarm_write(REG_VOLUME_CTRL, hook_alarm_volume_now);
		if (ret == OP_OK_QUERY) {
			LOG_DBG("alarm volume set to %u", hook_alarm_volume_now);
			hook_alarm_volume_sent = hook_alarm_volume_now;
			hook_alarm_report_online();
		} else {
			hook_alarm_log_failure("volume write");
		}
		return;
	}

	/* Rule 2: value already in sync, keep the link supervised. */
	if ((now - last_exchange_ms) < HOOK_ALARM_LIVENESS_PERIOD_MS) {
		return;
	}
	last_exchange_ms = now;

	ret = hook_alarm_write(REG_VOLUME_CTRL, hook_alarm_volume_now);
	if (ret == OP_OK_QUERY) {
		hook_alarm_report_online();
	} else {
		hook_alarm_log_failure("keepalive write");
	}
}

/*
 * Power-on self test of the alarm module: play test tone 1 of the module,
 * then flush the module state so the first real command starts clean.
 */
static void hook_alarm_power_on_self_test(void)
{
	uint32_t ret;

	LOG_INF("[POST] alarm module self test");
	k_msleep(2500);

	ret = hook_alarm_write(0x2303U, 0x0001U);
	GPIO_WRITE(pwr_led_gpio, 1);
	if (ret == OP_OK_QUERY) {
		LOG_INF("[POST] sound & light test command accepted");
	} else {
		LOG_ERR("[POST] sound & light test command failed");
	}

	k_msleep(5500);
	(void)hook_alarm_write(0x6003U, 0x0000U);
	GPIO_WRITE(pwr_led_gpio, 0);

	hook_reg_input_set(STATUS_LED_SWITCH, 0U);
	hook_reg_input_set(STATUS_BUZZER, 0U);

	LOG_INF("[POST] alarm module self test done");
}

static void hook_alarm_thread(void *a, void *b, void *c)
{
	static uint32_t last_logic_ms;

	(void)a;
	(void)b;
	(void)c;

	osal_notify_register_current(&hook_alarm_notify);

	hook_alarm_power_on_self_test();

	for (;;) {
		/* Liveness check-in first: it must happen in soft standby too,
		 * where only the alarm logic itself is parked (see hook_health.h).
		 */
		hook_health_task_alive(HOOK_TASK_ALIVE_ALARM);

		if (is_soft_standby != 1U) {
			hook_alarm_volume_sync();

			/* The blink pattern has its own 250 ms stepper and is
			 * evaluated on every pass (latest firmware behaviour).
			 */
			hook_alarm_led_logic();

			if (hook_get_time_ms() - last_logic_ms >=
			    HOOK_ALARM_LOGIC_PERIOD_MS) {
				last_logic_ms = hook_get_time_ms();
				hook_alarm_buzzer_logic();
			}
		}

		k_msleep(50);
	}
}

K_THREAD_STACK_DEFINE(hook_alarm_stack, 4096);
static struct k_thread hook_alarm_thread_obj;

void hook_alarm_start(void)
{
	osal_notify_init(&hook_alarm_notify);
	hook_alarm_force_volume_sync();

	k_thread_create(&hook_alarm_thread_obj, hook_alarm_stack,
			K_THREAD_STACK_SIZEOF(hook_alarm_stack),
			hook_alarm_thread, NULL, NULL, NULL,
			OSAL_PRIO_NORMAL1, 0, K_NO_WAIT);
	k_thread_name_set(&hook_alarm_thread_obj, "AlarmTask");
}
