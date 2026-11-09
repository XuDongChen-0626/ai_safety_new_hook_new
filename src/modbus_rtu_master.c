/*
 * modbus_rtu_master.c - Zephyr Modbus RTU clients for usart6/usart7.
 *
 * BMS:        usart6 (RS485), 9600 8N1, unit id 2
 * sound/light: uart7  (RS485), 9600 8N1, unit id 1
 */
#include "modbus_rtu_master.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/modbus/modbus.h>

#include "system_def.h"

LOG_MODULE_REGISTER(hook_modbus_master, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_BMS_MODBUS_NODE   DT_NODELABEL(rs485_usart6)
#define HOOK_ALARM_MODBUS_NODE DT_NODELABEL(rs485_uart7)

/* The old masters used a 500 ms response timeout. */
#define HOOK_MODBUS_RX_TIMEOUT_US (500U * USEC_PER_MSEC)

static const char hook_bms_iface_name[] = DEVICE_DT_NAME(HOOK_BMS_MODBUS_NODE);
#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
static const char hook_alarm_iface_name[] =
	DEVICE_DT_NAME(HOOK_ALARM_MODBUS_NODE);
#endif

static struct modbus_iface_param hook_client_param = {
	.mode = MODBUS_MODE_RTU,
	.rx_timeout = HOOK_MODBUS_RX_TIMEOUT_US,
	.serial = {
		.baud = 9600U,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
	},
};

static int hook_bms_iface = -1;
#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
static int hook_alarm_iface = -1;
#endif

static int hook_modbus_client_init(const char *iface_name, int *iface_out)
{
	int iface;
	int ret;

	iface = modbus_iface_get_by_name(iface_name);
	if (iface < 0) {
		return iface;
	}

	ret = modbus_init_client(iface, hook_client_param);
	if (ret == 0) {
		*iface_out = iface;
	}

	return ret;
}

int hook_modbus_master_init(void)
{
	int first_error = 0;
	int ret;

	ret = hook_modbus_client_init(hook_bms_iface_name, &hook_bms_iface);
	if (ret != 0) {
		LOG_ERR("BMS Modbus client init failed on usart6: %d", ret);
		first_error = ret;
	} else {
		LOG_INF("BMS Modbus RTU client ready on usart6 (9600)");
	}

#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
	ret = hook_modbus_client_init(hook_alarm_iface_name,
				      &hook_alarm_iface);
	if (ret != 0) {
		LOG_ERR("Alarm Modbus client init failed on uart7: %d", ret);
		if (first_error == 0) {
			first_error = ret;
		}
	} else {
		LOG_INF("Sound & light Modbus RTU client ready on uart7 (9600)");
	}
#endif

	return first_error;
}

int hook_modbus_bms_read_holding(uint16_t address, uint16_t *value)
{
	if (value == NULL) {
		return -EINVAL;
	}

	if (hook_bms_iface < 0) {
		return -ENODEV;
	}

	return modbus_read_holding_regs(hook_bms_iface, SLAVE_BMS_ID, address,
					value, 1U);
}

#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
int hook_modbus_alarm_write_holding(uint16_t address, uint16_t value)
{
	if (hook_alarm_iface < 0) {
		return -ENODEV;
	}

	return modbus_write_holding_reg(hook_alarm_iface, SLAVE_ALARM_ID, address,
					value);
}
#endif
