/*
 * modbus_rtu_master.c - Zephyr Modbus RTU master ports for usart6/usart7.
 *
 * Replaces init_modbus_master() calls of application/app_bms_alarm.c, which
 * used two HAL UART handles (huart6 for the BMS and huart7 for the sound &
 * light alarm module).
 */
#include "modbus_rtu_master.h"

#include "am_modbus_port.h"
#include "system_def.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hook_modbus_master, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_MASTER_BUFFER_WORDS 128U

static modbus_uart_t hook_bms_uart = {
	.dev = DEVICE_DT_GET(DT_NODELABEL(usart6)),
	.baudrate = 9600U,
	.en_gpio = NULL,
};

#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
static modbus_uart_t hook_alarm_uart = {
	.dev = DEVICE_DT_GET(DT_NODELABEL(uart7)),
	.baudrate = 9600U,
	.en_gpio = NULL,
};
#endif

modbusHandler_t hook_bms_master;

#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
modbusHandler_t hook_alarm_master;
#endif

static uint16_t hook_bms_master_buf[HOOK_MASTER_BUFFER_WORDS];

#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
static uint16_t hook_alarm_master_buf[HOOK_MASTER_BUFFER_WORDS];
#endif

static void hook_modbus_master_setup(modbusHandler_t *handler,
				     modbus_uart_t *port,
				     uint16_t *buffer,
				     uint16_t buffer_words)
{
	memset(buffer, 0, buffer_words * sizeof(buffer[0]));

	handler->uModbusType = MB_MASTER;
	handler->port = port;
	handler->u8id = 0U;
	handler->u16timeOut = 500U;
	handler->en_gpio = NULL;
	handler->u16regs = buffer;
	handler->u16regsize = buffer_words;
	handler->xTypeHW = USART_HW;

	ModbusInit(handler);
	ModbusStart(handler);
}

void hook_modbus_master_init(void)
{
	hook_modbus_master_setup(&hook_bms_master, &hook_bms_uart,
				 hook_bms_master_buf,
				 HOOK_MASTER_BUFFER_WORDS);
	LOG_INF("BMS Modbus RTU master initialised on usart6 (9600)");

#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
	hook_modbus_master_setup(&hook_alarm_master, &hook_alarm_uart,
				 hook_alarm_master_buf,
				 HOOK_MASTER_BUFFER_WORDS);
	LOG_INF("Sound & light Modbus RTU master initialised on uart7 (9600)");
#endif
}
