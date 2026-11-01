/*
 * modbus_rtu_master.h - hook Modbus RTU master ports.
 *
 * Two Zephyr Modbus clients:
 *   - BMS        : usart6 (RS485), 9600 8N1, slave id 2
 *   - alarm module: uart7 (RS485), 9600 8N1, slave id 1
 */
#ifndef HOOK_MODBUS_RTU_MASTER_H
#define HOOK_MODBUS_RTU_MASTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* @return 0 when all enabled client interfaces are ready. */
int hook_modbus_master_init(void);

/* FC03: read one BMS holding register. */
int hook_modbus_bms_read_holding(uint16_t address, uint16_t *value);

#if defined(CONFIG_HOOK_ALARM_MODULE_ENABLE)
/* FC06: write one sound & light alarm holding register. */
int hook_modbus_alarm_write_holding(uint16_t address, uint16_t value);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HOOK_MODBUS_RTU_MASTER_H */
