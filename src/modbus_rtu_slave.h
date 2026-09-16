/*
 * modbus_rtu_slave.h - host controller Modbus RTU slave port on uart8.
 */
#ifndef HOOK_MODBUS_RTU_SLAVE_H
#define HOOK_MODBUS_RTU_SLAVE_H

#include <stdint.h>

#include "Modbus.h"

#ifdef __cplusplus
extern "C" {
#endif

extern modbusHandler_t hook_host_slave;

/*
 * Bind the slave to the register store of app/hook_modbus and start the
 * RTU server thread.
 */
void hook_modbus_slave_init(uint16_t *holding_regs, uint16_t *input_regs,
			    uint16_t reg_count, uint8_t slave_id);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_MODBUS_RTU_SLAVE_H */
