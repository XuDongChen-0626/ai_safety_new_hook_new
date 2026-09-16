/*
 * hook_register.h - thread safe access to the hook Modbus register store.
 *
 * Replaces MB_Reg_Get()/MB_Reg_Set()/MB_Reg_SetBits() of the original
 * application/modbus_rtu_server_interface.c. The two arrays are handed to
 * component/am_modbus as the slave's holding/input register images, so every
 * accessor below works directly on the data served to the host.
 */
#ifndef HOOK_REGISTER_H
#define HOOK_REGISTER_H

#include <stdint.h>

#include "system_def.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOOK_REGISTER_COUNT REGS_TOTAL_NUM

uint16_t hook_reg_get(uint16_t index);
void hook_reg_set(uint16_t index, uint16_t value);

/* Read-modify-write helpers (atomic with respect to ISRs/threads). */
void hook_reg_set_bits(uint16_t index, uint16_t mask, uint16_t value);
void hook_reg_set_error_bits(uint16_t mask);
void hook_reg_clear_error_bits(uint16_t mask);
uint16_t hook_reg_get_error_bits(void);

/* Slave register images (holding / input). */
uint16_t *hook_reg_holding(void);
uint16_t *hook_reg_input(void);

/* Clear everything and apply the power-on defaults of the old firmware. */
void hook_reg_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_REGISTER_H */
