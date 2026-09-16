/*
 * modbus_rtu_slave.c - Zephyr Modbus RTU slave port on uart8 (RS485).
 *
 * Replaces init_modbus_slave(&modbus_rtu_server, &huart8,
 * FORWARD_SLAVE_ADDR) of application/app_hook_task.c. The crane host
 * controller is the Modbus master on this bus.
 */
#include "modbus_rtu_slave.h"

#include "am_modbus_port.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hook_modbus_slave, CONFIG_LOG_DEFAULT_LEVEL);

static modbus_uart_t hook_host_uart = {
	.dev = DEVICE_DT_GET(DT_NODELABEL(uart8)),
	.baudrate = 9600U,
	.en_gpio = NULL,
};

modbusHandler_t hook_host_slave;

void hook_modbus_slave_init(uint16_t *holding_regs, uint16_t *input_regs,
			    uint16_t reg_count, uint8_t slave_id)
{
	hook_host_slave.uModbusType = MB_SLAVE;
	hook_host_slave.u8id = slave_id;
	hook_host_slave.port = &hook_host_uart;
	hook_host_slave.en_gpio = NULL;
	hook_host_slave.u16regs = holding_regs;
	hook_host_slave.u16inputregs = input_regs;
	hook_host_slave.u16regsize = reg_count;
	hook_host_slave.u16inputregsize = reg_count;
	hook_host_slave.u16timeOut = 1000U;
	hook_host_slave.xTypeHW = USART_HW;

	ModbusInit(&hook_host_slave);
	ModbusStart(&hook_host_slave);

	LOG_INF("Modbus RTU slave address %u ready on uart8 (RS485, %u regs)",
		slave_id, reg_count);
}
