/*
 * modbus_rtu_master.h - hook Modbus RTU master ports.
 *
 * Two masters share the am_modbus library port:
 *   - BMS        : usart6 (RS485), 9600 8N1, slave id 2
 *   - alarm module: uart7 (RS485), 9600 8N1, slave id 1
 */
#ifndef HOOK_MODBUS_RTU_MASTER_H
#define HOOK_MODBUS_RTU_MASTER_H

#include "Modbus.h"

#ifdef __cplusplus
extern "C" {
#endif

extern modbusHandler_t hook_bms_master;
#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
extern modbusHandler_t hook_alarm_master;
#endif

void hook_modbus_master_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_MODBUS_RTU_MASTER_H */
