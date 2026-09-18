/*
 * hook_register.c - hook Modbus register table on top of modbus_data_model.
 *
 * The old firmware kept two flat uint16_t arrays (holding / input) that were
 * handed to the RTU server; registers had no name, no access flags and no
 * default, and the power schedule was lost on every reboot. This file
 * registers the same addresses as a named data model table, which gives one
 * single source of truth for the host interface, the firmware and the shell,
 * plus optional flash persistence for the power on/off schedule.
 */
#include "hook_register.h"

#include "hook_register_map.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "modbus_data_model.h"

#if defined(CONFIG_MODBUS_DATA_MODEL_STORE) && \
	defined(CONFIG_MODBUS_DATA_MODEL_STORE_FLASH)
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>

#include "modbus_data_model_store.h"
#include "modbus_data_model_store_flash.h"
#define HOOK_REGISTER_STORE 1
#endif

LOG_MODULE_REGISTER(hook_register, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * Holding register table (FC03 / FC06 / FC16): everything the host / cloud
 * writes - commands, configuration and the one shot RTC calibration command.
 *
 * Only the registers this product actually uses are listed; any other address
 * of the 0..255 address space reads as 0 and ignores writes (see
 * docs/寄存器映射.md).
 */
static struct modbus_data_model_holding hook_holding_table[] = {
	/* --- host commands (0..99) --- */
	{ .name = "cmd_led_switch", .addr = CMD_LED_SWITCH,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "cmd_buzzer_7m", .addr = CMD_BUZZER_7M,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "cmd_buzzer_3m", .addr = CMD_BUZZER_3M,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "cmd_volume", .addr = CMD_VOLUME,
	  .default_value = DEFAULT_VOLUME, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "cmd_host_heartbeat", .addr = CMD_HOST_HEARTBEAT,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },

	/* --- configuration (16..): the schedule survives a reboot --- */
	{ .name = "power_off_time", .addr = POWER_OFF_TIME,
	  .default_value = POWER_OFF_DEFAULT, .flags = MODBUS_REG_ACCESS_RW_PERSISTENT },
	{ .name = "power_on_time", .addr = POWER_ON_TIME,
	  .default_value = POWER_ON_DEFAULT, .flags = MODBUS_REG_ACCESS_RW_PERSISTENT },
	{ .name = "stm32_standby_enable", .addr = STM32_STANDBY_ENABLE,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "soft_standby_enable", .addr = SOFT_STANDBY_ENABLE,
	  .default_value = 1U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "heartbeat_enable", .addr = HEARTBEAT_ENABLE,
	  .default_value = 1U, .flags = MODBUS_REG_ACCESS_RW },

	/* --- RTC calibration command (one shot, consumed by hook_rtc) --- */
	{ .name = "rtc_cal_year", .addr = RTC_CAL_YEAR,
	  .default_value = RTC_CAL_YEAR_DEFAULT, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "rtc_cal_month_day", .addr = RTC_CAL_MONTH_DAY,
	  .default_value = RTC_CAL_MONTH_DAY_DEFAULT, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "rtc_cal_hhmm", .addr = RTC_CAL_HHMM,
	  .default_value = RTC_CAL_HHMM_DEFAULT, .flags = MODBUS_REG_ACCESS_RW },
};

/* Input register table (FC04): everything the host / cloud reads. The
 * firmware publishes device status, BMS telemetry and the current time here;
 * the host can only read them (FC04 has no write function code).
 */
static struct modbus_data_model_input hook_input_table[] = {
	{ .name = "status_work_mode", .addr = STATUS_WORK_MODE,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_error_code", .addr = REG_ERROR_CODE,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_led_switch", .addr = STATUS_LED_SWITCH,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_buzzer", .addr = STATUS_BUZZER,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_bms_battery", .addr = STATUS_BMS_BATTERY,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_bms_total_voltage", .addr = STATUS_BMS_TOTAL_VOLTAGE,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_bms_total_current", .addr = STATUS_BMS_TOTAL_CURRENT,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_bms_is_charge", .addr = STATUS_BMS_IS_CHARGE,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_bms_remain_discharge",
	  .addr = STATUS_BMS_REMAIN_DISCHARGE_TIME,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_bms_remain_charge",
	  .addr = STATUS_BMS_REMAIN_CHARGE_TIME,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "status_bms_protect", .addr = STATUS_BMS_PROTECT_STATUS,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },

	/* current time, published by app/hook_rtc */
	{ .name = "rtc_year", .addr = INPUT_RTC_YEAR,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "rtc_month_day", .addr = INPUT_RTC_MONTH_DAY,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
	{ .name = "rtc_hhmm", .addr = INPUT_RTC_TIME_HHMM,
	  .default_value = 0U, .flags = MODBUS_REG_ACCESS_RW },
};

static struct modbus_data_model_table hook_register_table = {
	.holdings = hook_holding_table,
	.holding_count = ARRAY_SIZE(hook_holding_table),
	.holding_address_size = HOOK_REGISTER_COUNT,
	.inputs = hook_input_table,
	.input_count = ARRAY_SIZE(hook_input_table),
	.input_address_size = HOOK_REGISTER_COUNT,
};

/* Serialises the read-modify-write helpers below (the data model serialises
 * the individual accesses, not a read followed by a write).
 */
static K_MUTEX_DEFINE(hook_reg_rmw_lock);

uint16_t hook_reg_get(uint16_t index)
{
	uint16_t value = 0U;

	if (index >= HOOK_REGISTER_COUNT) {
		return 0U;
	}

	if (modbus_data_model_read_holding(index, &value) != 0) {
		return 0U;
	}

	return value;
}

void hook_reg_set(uint16_t index, uint16_t value)
{
	if (index >= HOOK_REGISTER_COUNT) {
		return;
	}

	if (modbus_data_model_write_holding(index, value) != 0) {
		LOG_DBG("holding %u is not mapped, write ignored", index);
	}
}

void hook_reg_set_bits(uint16_t index, uint16_t mask, uint16_t value)
{
	uint16_t current;

	if (index >= HOOK_REGISTER_COUNT) {
		return;
	}

	(void)k_mutex_lock(&hook_reg_rmw_lock, K_FOREVER);
	current = hook_reg_get(index);
	hook_reg_set(index, (uint16_t)((current & ~mask) | (value & mask)));
	k_mutex_unlock(&hook_reg_rmw_lock);
}

uint16_t hook_reg_input_get(uint16_t index)
{
	uint16_t value = 0U;

	if (index >= HOOK_REGISTER_COUNT) {
		return 0U;
	}

	if (modbus_data_model_read_input(index, &value) != 0) {
		return 0U;
	}

	return value;
}

void hook_reg_input_set(uint16_t index, uint16_t value)
{
	if (index >= HOOK_REGISTER_COUNT) {
		return;
	}

	if (modbus_data_model_write_input(index, value) != 0) {
		LOG_DBG("input %u is not mapped, write ignored", index);
	}
}

void hook_reg_input_set_bits(uint16_t index, uint16_t mask, uint16_t value)
{
	uint16_t current;

	if (index >= HOOK_REGISTER_COUNT) {
		return;
	}

	(void)k_mutex_lock(&hook_reg_rmw_lock, K_FOREVER);
	current = hook_reg_input_get(index);
	hook_reg_input_set(index, (uint16_t)((current & ~mask) | (value & mask)));
	k_mutex_unlock(&hook_reg_rmw_lock);
}

/* --- error code helpers (input register REG_ERROR_CODE) ------------- */

uint16_t hook_reg_get_error_bits(void)
{
	return hook_reg_input_get(REG_ERROR_CODE);
}

void hook_reg_set_error_bits(uint16_t mask)
{
	hook_reg_input_set_bits(REG_ERROR_CODE, mask, mask);
}

void hook_reg_clear_error_bits(uint16_t mask)
{
	hook_reg_input_set_bits(REG_ERROR_CODE, mask, 0U);
}

/* --- component/am_modbus slave callbacks --------------------------- */

int8_t hook_reg_model_read(uint16_t address, uint16_t *value, void *user_data)
{
	ARG_UNUSED(user_data);

	if (value == NULL) {
		return (int8_t)-EINVAL;
	}

	if (modbus_data_model_read_holding(address, value) != 0) {
		return (int8_t)-EIO;
	}

	return 0;
}

int8_t hook_reg_model_write(uint16_t address, uint16_t value, void *user_data)
{
	ARG_UNUSED(user_data);

	if (modbus_data_model_write_holding(address, value) != 0) {
		return (int8_t)-EIO;
	}

	return 0;
}

int8_t hook_reg_model_read_input(uint16_t address, uint16_t *value,
				 void *user_data)
{
	ARG_UNUSED(user_data);

	if (value == NULL) {
		return (int8_t)-EINVAL;
	}

	if (modbus_data_model_read_input(address, value) != 0) {
		return (int8_t)-EIO;
	}

	return 0;
}

#if defined(HOOK_REGISTER_STORE)
static struct modbus_data_model_store_backend hook_register_store_backend;

/*
 * The store lives on the on-board W25Q64 QSPI NOR flash (modbus-store
 * partition, 8 KiB): two 4 KiB banks, each exactly one NOR sector, so the
 * double bank really protects the previously saved values against a power
 * loss in the middle of a save. On the internal flash (128 KiB sectors) the
 * two banks would share one sector, which is why the store moved to QSPI.
 */
static int hook_register_store_init(void)
{
	static const struct modbus_data_model_flash_store_config flash_cfg = {
		.flash_area_id =
			DT_FIXED_PARTITION_ID(DT_NODELABEL(modbus_store_partition)),
	};
	static const struct modbus_data_model_store_config store_cfg = {
		.backend = &hook_register_store_backend,
		.save_delay_ms = 3000U,
	};
	int ret;

	ret = modbus_data_model_flash_store_backend_init(&flash_cfg,
							 &hook_register_store_backend);
	if (ret != 0) {
		return ret;
	}

	return modbus_data_model_store_init(&store_cfg);
}
#endif /* HOOK_REGISTER_STORE */

int hook_register_init(void)
{
	int ret;

	ret = modbus_data_model_register_table(&hook_register_table);
	if (ret != 0) {
		LOG_ERR("register table rejected: %d", ret);
		return ret;
	}

	ret = modbus_data_model_init();
	if (ret != 0) {
		LOG_ERR("data model init failed: %d", ret);
		return ret;
	}

#if defined(HOOK_REGISTER_STORE)
	ret = hook_register_store_init();
	if (ret != 0) {
		/* Only the persistence of the power schedule is affected. */
		LOG_WRN("persistent store unavailable (%d), defaults stay in RAM", ret);
	} else {
		LOG_INF("register table ready (%u holding / %u input, schedule persisted)",
			(unsigned int)ARRAY_SIZE(hook_holding_table),
			(unsigned int)ARRAY_SIZE(hook_input_table));
	}
#else
	LOG_INF("register table ready (%u holding / %u input, no persistence)",
		(unsigned int)ARRAY_SIZE(hook_holding_table),
		(unsigned int)ARRAY_SIZE(hook_input_table));
#endif

	return 0;
}
