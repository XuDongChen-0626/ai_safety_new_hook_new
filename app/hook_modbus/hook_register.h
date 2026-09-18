/*
 * hook_register.h - access to the hook Modbus register map.
 *
 * Replaces MB_Reg_Get()/MB_Reg_Set()/MB_Reg_SetBits() of the original
 * application/modbus_rtu_server_interface.c.
 *
 * The values themselves live in module/modbus_data_model (git submodule):
 * the product table with names, addresses, access flags and defaults is
 * registered from hook_register.c, and this file is the product API on top
 * of it. Every accessor is thread safe - the data model serialises accesses
 * with its own mutex - and the RTU slave is served from the very same table
 * through the hook_reg_model_* callbacks below, so the host and the firmware
 * always see identical values.
 */
#ifndef HOOK_REGISTER_H
#define HOOK_REGISTER_H

#include <stdint.h>

#include "system_def.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOOK_REGISTER_COUNT REGS_TOTAL_NUM

/* Holding registers (FC03 / FC06 / FC16): written by the host / cloud.
 * Unmapped addresses read as 0 and discard writes: the product table only
 * lists the registers that have a meaning for this controller.
 */
uint16_t hook_reg_get(uint16_t index);
void hook_reg_set(uint16_t index, uint16_t value);

/* Read-modify-write helper on a holding register (atomic among themselves). */
void hook_reg_set_bits(uint16_t index, uint16_t mask, uint16_t value);

/* Input registers (FC04): published by the firmware, read only for the host.
 * The firmware writes them, so they are also writable from the data model.
 */
uint16_t hook_reg_input_get(uint16_t index);
void hook_reg_input_set(uint16_t index, uint16_t value);
void hook_reg_input_set_bits(uint16_t index, uint16_t mask, uint16_t value);

/* Error code helpers. The error code lives in the input register area
 * (REG_ERROR_CODE), so these are thin wrappers around the input accessors.
 */
void hook_reg_set_error_bits(uint16_t mask);
void hook_reg_clear_error_bits(uint16_t mask);
uint16_t hook_reg_get_error_bits(void);

/* Register table + data model (+ optional persistent store) bring-up. */
int hook_register_init(void);

/* Register access callbacks handed to component/am_modbus (slave side). */
int8_t hook_reg_model_read(uint16_t address, uint16_t *value, void *user_data);
int8_t hook_reg_model_write(uint16_t address, uint16_t value, void *user_data);
int8_t hook_reg_model_read_input(uint16_t address, uint16_t *value,
				 void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_REGISTER_H */
