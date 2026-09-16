/*
 * hook_modbus.c - hook Modbus layer bring-up.
 */
#include "hook_modbus.h"

#include "hook_register.h"
#include "modbus_rtu_master.h"
#include "modbus_rtu_slave.h"
#include "system_def.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hook_modbus, CONFIG_LOG_DEFAULT_LEVEL);

void hook_modbus_init(void)
{
	hook_reg_reset();

	/* The hook is the RTU slave of the crane host controller (uart8). */
	hook_modbus_slave_init(hook_reg_holding(), hook_reg_input(),
			       HOOK_REGISTER_COUNT, FORWARD_SLAVE_ADDR);

	/* External devices polled by this controller. */
#if defined(CONFIG_HOOK_BMS_ENABLE) || defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
	hook_modbus_master_init();
#endif
}
