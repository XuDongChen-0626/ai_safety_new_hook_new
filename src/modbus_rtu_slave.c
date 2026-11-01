/*
 * modbus_rtu_slave.c - Zephyr Modbus RTU server on uart8 (RS485).
 *
 * The crane host controller is the Modbus master on this bus. Holding and
 * input registers are served directly from the hook register data model.
 */
#include "modbus_rtu_slave.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/sys/util.h>

#include "hook_register.h"

LOG_MODULE_REGISTER(hook_modbus_slave, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_HOST_MODBUS_NODE DT_NODELABEL(rs485_uart8)

/*
 * Coil functions are not part of the hook host protocol. Keep the same small
 * shadow area the previous component exposed, instead of dereferencing an
 * absent coil table.
 */
static uint16_t hook_host_coils[16];

static int hook_slave_coil_read(uint16_t addr, bool *state)
{
	if ((state == NULL) || (addr >= (ARRAY_SIZE(hook_host_coils) * 16U))) {
		return -ENOTSUP;
	}

	*state = (hook_host_coils[addr / 16U] & BIT(addr % 16U)) != 0U;
	return 0;
}

static int hook_slave_coil_write(uint16_t addr, bool state)
{
	if (addr >= (ARRAY_SIZE(hook_host_coils) * 16U)) {
		return -ENOTSUP;
	}

	if (state) {
		hook_host_coils[addr / 16U] |= BIT(addr % 16U);
	} else {
		hook_host_coils[addr / 16U] &= (uint16_t)~BIT(addr % 16U);
	}

	return 0;
}

static int hook_slave_holding_read(uint16_t addr, uint16_t *reg)
{
	uint16_t value = 0U;

	if (reg == NULL) {
		return -EINVAL;
	}

	/*
	 * Preserve the old wire behaviour: an unmapped address reads as zero
	 * and does not turn a valid FC03/FC04 request into an exception.
	 */
	(void)hook_reg_model_read(addr, &value, NULL);
	*reg = value;
	return 0;
}

static int hook_slave_holding_write(uint16_t addr, uint16_t reg)
{
	/*
	 * The data model rejects read-only or unmapped addresses. The old
	 * server discarded those writes but still acknowledged the request, so
	 * do not convert them into an illegal-address exception.
	 */
	(void)hook_reg_model_write(addr, reg, NULL);
	return 0;
}

static int hook_slave_input_read(uint16_t addr, uint16_t *reg)
{
	uint16_t value = 0U;

	if (reg == NULL) {
		return -EINVAL;
	}

	(void)hook_reg_model_read_input(addr, &value, NULL);
	*reg = value;
	return 0;
}

static struct modbus_user_callbacks hook_slave_callbacks = {
	.coil_rd = hook_slave_coil_read,
	.coil_wr = hook_slave_coil_write,
	.input_reg_rd = hook_slave_input_read,
	.holding_reg_rd = hook_slave_holding_read,
	.holding_reg_wr = hook_slave_holding_write,
};

static struct modbus_iface_param hook_slave_param = {
	.mode = MODBUS_MODE_RTU,
	.server = {
		.user_cb = &hook_slave_callbacks,
		.unit_id = 1U,
	},
	.serial = {
		.baud = 9600U,
		.parity = UART_CFG_PARITY_NONE,
		/* Keep the old 8N1 RS485 timing, not Modbus's parity-none 8N2. */
		.stop_bits = UART_CFG_STOP_BITS_1,
	},
};

int hook_modbus_slave_init(uint16_t reg_count, uint8_t slave_id)
{
	static const char iface_name[] = DEVICE_DT_NAME(HOOK_HOST_MODBUS_NODE);
	int iface;
	int ret;

	iface = modbus_iface_get_by_name(iface_name);
	if (iface < 0) {
		LOG_ERR("Host Modbus interface %s unavailable: %d", iface_name,
			iface);
		return iface;
	}

	hook_slave_param.server.unit_id = slave_id;
	ret = modbus_init_server(iface, hook_slave_param);
	if (ret != 0) {
		LOG_ERR("Host Modbus RTU server init failed: %d", ret);
		return ret;
	}

	LOG_INF("Modbus RTU server address %u ready on uart8 (RS485, %u regs)",
		slave_id, reg_count);
	return 0;
}
