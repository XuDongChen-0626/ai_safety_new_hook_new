/*
 * hook_bms.c - BMS Modbus RTU master polling thread.
 *
 * The BMS (slave address 2) is polled on usart6:
 *   - every  500 ms: remaining discharge time, protect status, ditto charge
 *   - every  10 s  : battery level, total voltage, total current; the
 *                    remaining times are averaged over the last samples.
 */
#include "hook_bms.h"

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

LOG_MODULE_REGISTER(hook_bms, CONFIG_LOG_DEFAULT_LEVEL);

typedef enum {
	READ_BATT_LEVEL = 0,
	READ_REMAIN_DISCHARGE,
	READ_TOTAL_VOLTAGE,
	READ_IS_CHARGING,
	READ_REMAIN_CHARGE,
	READ_TOTAL_CURRENT,
	READ_PROTECT_STATUS,
	READ_MSG_COUNT
} hook_bms_read_idx_t;

static uint16_t hook_bms_read_results[READ_MSG_COUNT];

static modbus_t hook_bms_read_telegrams[READ_MSG_COUNT] = {
	[READ_BATT_LEVEL] = {.u8id = SLAVE_BMS_ID, .u8fct = MB_FC_READ_REGISTERS,
			     .u16RegAdd = REG_BATTERY_LEVEL, .u16CoilsNo = 1U,
			     .u16reg = &hook_bms_read_results[READ_BATT_LEVEL]},
	[READ_REMAIN_DISCHARGE] = {.u8id = SLAVE_BMS_ID, .u8fct = MB_FC_READ_REGISTERS,
				   .u16RegAdd = REG_REMAIN_DISCHARGE, .u16CoilsNo = 1U,
				   .u16reg = &hook_bms_read_results[READ_REMAIN_DISCHARGE]},
	[READ_TOTAL_VOLTAGE] = {.u8id = SLAVE_BMS_ID, .u8fct = MB_FC_READ_REGISTERS,
				.u16RegAdd = REG_TOTAL_VOLTAGE, .u16CoilsNo = 1U,
				.u16reg = &hook_bms_read_results[READ_TOTAL_VOLTAGE]},
	[READ_IS_CHARGING] = {.u8id = SLAVE_BMS_ID, .u8fct = MB_FC_READ_REGISTERS,
			      .u16RegAdd = REG_IS_CHARGING, .u16CoilsNo = 1U,
			      .u16reg = &hook_bms_read_results[READ_IS_CHARGING]},
	[READ_REMAIN_CHARGE] = {.u8id = SLAVE_BMS_ID, .u8fct = MB_FC_READ_REGISTERS,
				.u16RegAdd = REG_REMAIN_CHARGE, .u16CoilsNo = 1U,
				.u16reg = &hook_bms_read_results[READ_REMAIN_CHARGE]},
	[READ_TOTAL_CURRENT] = {.u8id = SLAVE_BMS_ID, .u8fct = MB_FC_READ_REGISTERS,
				.u16RegAdd = REG_TOTAL_CURRENT, .u16CoilsNo = 1U,
				.u16reg = &hook_bms_read_results[READ_TOTAL_CURRENT]},
	[READ_PROTECT_STATUS] = {.u8id = SLAVE_BMS_ID, .u8fct = MB_FC_READ_REGISTERS,
				 .u16RegAdd = REG_PROTECT_STATUS, .u16CoilsNo = 1U,
				 .u16reg = &hook_bms_read_results[READ_PROTECT_STATUS]},
};

static osal_notify_t hook_bms_notify;
static uint32_t hook_bms_discharge_samples[BMS_SAMPLE_BUFFER_SIZE];
static uint32_t hook_bms_charge_samples[BMS_SAMPLE_BUFFER_SIZE];
static uint8_t hook_bms_discharge_idx;
static uint8_t hook_bms_discharge_count;
static uint8_t hook_bms_charge_idx;
static uint8_t hook_bms_charge_count;

static uint32_t hook_bms_query(hook_bms_read_idx_t idx)
{
	ModbusQuery(&hook_bms_master, hook_bms_read_telegrams[idx]);

	return osal_notify_wait(&hook_bms_notify, MODBUS_WAIT_TIMEOUT_MS);
}

/*
 * Fast BMS round (every 500 ms). Returns true when at least one query was
 * answered; any answered exchange proves that the BMS is reachable and is
 * what keeps the offline judgement (see hook_bms_thread) satisfied.
 */
static bool hook_bms_read_remaining_times(void)
{
	bool any_ok = false;
	uint16_t value;

	if (hook_bms_query(READ_REMAIN_DISCHARGE) == OP_OK_QUERY) {
		any_ok = true;
		value = hook_bms_read_results[READ_REMAIN_DISCHARGE];
		if (value != 0xFFFFU) {
			hook_bms_discharge_samples[hook_bms_discharge_idx %
						   BMS_SAMPLE_VALID_COUNT] = value;
			hook_bms_discharge_idx++;
			if (hook_bms_discharge_count < BMS_SAMPLE_VALID_COUNT) {
				hook_bms_discharge_count++;
			}
		}
	} else {
		LOG_ERR_RATELIMIT("remaining discharge time read failed");
	}

	if (hook_bms_query(READ_PROTECT_STATUS) == OP_OK_QUERY) {
		any_ok = true;
		hook_reg_input_set(STATUS_BMS_PROTECT_STATUS,
			     hook_bms_read_results[READ_PROTECT_STATUS]);
		LOG_DBG_RATELIMIT("BMS protect status = 0x%04X",
				  hook_bms_read_results[READ_PROTECT_STATUS]);
	} else {
		LOG_ERR_RATELIMIT("BMS protect status read failed");
	}

	if (hook_bms_query(READ_REMAIN_CHARGE) == OP_OK_QUERY) {
		any_ok = true;
		value = hook_bms_read_results[READ_REMAIN_CHARGE];
		if (value != 0xFFFFU) {
			hook_bms_charge_samples[hook_bms_charge_idx %
						BMS_SAMPLE_VALID_COUNT] = value;
			hook_bms_charge_idx++;
			if (hook_bms_charge_count < BMS_SAMPLE_VALID_COUNT) {
				hook_bms_charge_count++;
			}
		}
	} else {
		LOG_ERR_RATELIMIT("remaining charge time read failed");
	}

	return any_ok;
}

static uint16_t hook_bms_average(const uint32_t *samples, uint8_t count)
{
	uint32_t sum = 0U;

	if (count == 0U) {
		return 0xFFFFU;
	}

	for (uint8_t i = 0U; i < count; i++) {
		sum += samples[i];
	}

	return (uint16_t)(sum / count);
}

static bool hook_bms_read_slow_values(void)
{
	bool any_ok = false;

	if (hook_bms_query(READ_BATT_LEVEL) == OP_OK_QUERY) {
		any_ok = true;
		hook_reg_input_set(STATUS_BMS_BATTERY,
			     hook_bms_read_results[READ_BATT_LEVEL]);
		LOG_DBG_RATELIMIT("BMS battery level = %u",
				  hook_bms_read_results[READ_BATT_LEVEL]);
	} else {
		LOG_ERR_RATELIMIT("BMS battery level read failed");
		hook_reg_input_set(STATUS_BMS_BATTERY, 0U);
	}

	if (hook_bms_query(READ_TOTAL_VOLTAGE) == OP_OK_QUERY) {
		any_ok = true;
		hook_reg_input_set(STATUS_BMS_TOTAL_VOLTAGE,
			     hook_bms_read_results[READ_TOTAL_VOLTAGE]);
		LOG_DBG_RATELIMIT("BMS total voltage = %u",
				  hook_bms_read_results[READ_TOTAL_VOLTAGE]);
	} else {
		LOG_ERR_RATELIMIT("BMS total voltage read failed");
	}

	if (hook_bms_query(READ_TOTAL_CURRENT) == OP_OK_QUERY) {
		any_ok = true;
		int16_t current = (int16_t)hook_bms_read_results[READ_TOTAL_CURRENT];

		if (current < 0) {
			hook_reg_input_set(STATUS_BMS_TOTAL_CURRENT,
				     (uint16_t)(-(int32_t)current));
			hook_reg_input_set(STATUS_BMS_IS_CHARGE, 0U); /* discharging */
		} else if (current > 0) {
			hook_reg_input_set(STATUS_BMS_TOTAL_CURRENT, (uint16_t)current);
			hook_reg_input_set(STATUS_BMS_IS_CHARGE, 1U); /* charging */
		} else {
			hook_reg_input_set(STATUS_BMS_IS_CHARGE, 2U); /* idle */
		}
		LOG_DBG_RATELIMIT("BMS current = %d, charging = %u", current,
				  hook_reg_input_get(STATUS_BMS_IS_CHARGE));
	} else {
		LOG_ERR_RATELIMIT("BMS total current read failed");
	}

	hook_reg_input_set(STATUS_BMS_REMAIN_DISCHARGE_TIME,
		     hook_bms_average(hook_bms_discharge_samples,
				      hook_bms_discharge_count));
	hook_reg_input_set(STATUS_BMS_REMAIN_CHARGE_TIME,
		     hook_bms_average(hook_bms_charge_samples,
				      hook_bms_charge_count));

	memset(hook_bms_discharge_samples, 0, sizeof(hook_bms_discharge_samples));
	memset(hook_bms_charge_samples, 0, sizeof(hook_bms_charge_samples));
	hook_bms_discharge_idx = 0U;
	hook_bms_discharge_count = 0U;
	hook_bms_charge_idx = 0U;
	hook_bms_charge_count = 0U;

	return any_ok;
}

static void hook_bms_thread(void *a, void *b, void *c)
{
	static uint32_t last_500ms;
	static uint32_t last_10s;
	uint32_t now;

	(void)a;
	(void)b;
	(void)c;

	osal_notify_register_current(&hook_bms_notify);

	for (;;) {
		bool any_ok = false;

		/* Check-in first: it means "this thread is alive", also while the
		 * polling itself is parked in soft standby. A check-in must never
		 * depend on whether the current mode has work to do, otherwise a
		 * normal soft standby would look like a hung thread.
		 */
		hook_health_task_alive(HOOK_TASK_ALIVE_BMS);

		/* The BMS keeps being polled in soft standby, exactly like the
		 * CubeMX firmware (modbus_bms_handle() had no standby check): the
		 * alarm outputs are off, but battery level / charge state stay
		 * observable and the health service keeps supervising the BMS for
		 * the whole standby window.
		 */
		now = hook_get_time_ms();

		if (now - last_500ms >= 500U) {
			last_500ms = now;
			any_ok = hook_bms_read_remaining_times();
		}

		if (now - last_10s >= 10000U) {
			last_10s = now;
			any_ok = hook_bms_read_slow_values() || any_ok;
		}

		/* Report every answered round. The online/offline decision (and
		 * the ERR_BMS_READ_FAIL bit that goes with it) belongs to the
		 * health service - see app/hook_health.
		 */
		if (any_ok) {
			hook_health_report(HOOK_HEALTH_EVENT_BMS);
		}

		k_msleep(100);
	}
}

K_THREAD_STACK_DEFINE(hook_bms_stack, 4096);
static struct k_thread hook_bms_thread_obj;

void hook_bms_start(void)
{
	osal_notify_init(&hook_bms_notify);

	k_thread_create(&hook_bms_thread_obj, hook_bms_stack,
			K_THREAD_STACK_SIZEOF(hook_bms_stack),
			hook_bms_thread, NULL, NULL, NULL,
			OSAL_PRIO_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_bms_thread_obj, "BmsMaster");
}
