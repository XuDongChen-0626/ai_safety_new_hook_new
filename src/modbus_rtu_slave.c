/*
 * modbus_rtu_slave.c - Zephyr Modbus RTU slave port on uart8 (RS485).
 *
 * Replaces init_modbus_slave(&modbus_rtu_server, &huart8,
 * FORWARD_SLAVE_ADDR) of application/app_hook_task.c. The crane host
 * controller is the Modbus master on this bus.
 */
#include "modbus_rtu_slave.h"

#include "am_modbus_port.h"
#include "hook_register.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hook_modbus_slave, CONFIG_LOG_DEFAULT_LEVEL);

static modbus_uart_t hook_host_uart = {
	.dev = DEVICE_DT_GET(DT_NODELABEL(uart8)),
	.baudrate = 9600U,
	.en_gpio = NULL,
};

/*
 * Coil functions (FC01/05/15) are not part of the hook host protocol. Keep a
 * small shadow array so that a stray coil request can never dereference NULL;
 * holding and input registers are served by the hook_register data model
 * callbacks instead of a flat array.
 */
static uint16_t hook_host_coils[16];

modbusHandler_t hook_host_slave;

void hook_modbus_slave_init(uint16_t reg_count, uint8_t slave_id)
{
	hook_host_slave.uModbusType = MB_SLAVE;
	hook_host_slave.u8id = slave_id;
	hook_host_slave.port = &hook_host_uart;
	hook_host_slave.en_gpio = NULL;
	hook_host_slave.u16regs = hook_host_coils;
	hook_host_slave.u16inputregs = NULL;
	hook_host_slave.u16regsize = reg_count;
	hook_host_slave.u16inputregsize = reg_count;
	hook_host_slave.u16timeOut = 1000U;
	hook_host_slave.xTypeHW = USART_HW;
	hook_host_slave.reg_read_cb = hook_reg_model_read;
	hook_host_slave.reg_write_cb = hook_reg_model_write;
	hook_host_slave.input_read_cb = hook_reg_model_read_input;
	hook_host_slave.reg_user_data = NULL;

	ModbusInit(&hook_host_slave);
	ModbusStart(&hook_host_slave);

	LOG_INF("Modbus RTU slave address %u ready on uart8 (RS485, %u regs)",
		slave_id, reg_count);
}
