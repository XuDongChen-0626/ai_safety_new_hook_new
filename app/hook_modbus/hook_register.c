/*
 * hook_register.c - hook Modbus register store.
 */
#include "hook_register.h"

#include "hook_register_map.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

static uint16_t hook_holding_regs[HOOK_REGISTER_COUNT];
static uint16_t hook_input_regs[HOOK_REGISTER_COUNT];

uint16_t hook_reg_get(uint16_t index)
{
	unsigned int key;
	uint16_t value;

	if (index >= HOOK_REGISTER_COUNT) {
		return 0U;
	}

	key = irq_lock();
	value = hook_holding_regs[index];
	irq_unlock(key);

	return value;
}

void hook_reg_set(uint16_t index, uint16_t value)
{
	unsigned int key;

	if (index >= HOOK_REGISTER_COUNT) {
		return;
	}

	key = irq_lock();
	hook_holding_regs[index] = value;
	irq_unlock(key);
}

void hook_reg_set_bits(uint16_t index, uint16_t mask, uint16_t value)
{
	unsigned int key;

	if (index >= HOOK_REGISTER_COUNT) {
		return;
	}

	key = irq_lock();
	hook_holding_regs[index] = (uint16_t)((hook_holding_regs[index] & ~mask) |
					      (value & mask));
	irq_unlock(key);
}

uint16_t hook_reg_get_error_bits(void)
{
	return hook_reg_get(REG_ERROR_CODE);
}

void hook_reg_set_error_bits(uint16_t mask)
{
	hook_reg_set_bits(REG_ERROR_CODE, mask, mask);
}

void hook_reg_clear_error_bits(uint16_t mask)
{
	hook_reg_set_bits(REG_ERROR_CODE, mask, 0U);
}

uint16_t *hook_reg_holding(void)
{
	return hook_holding_regs;
}

uint16_t *hook_reg_input(void)
{
	return hook_input_regs;
}

void hook_reg_reset(void)
{
	memset(hook_holding_regs, 0, sizeof(hook_holding_regs));
	memset(hook_input_regs, 0, sizeof(hook_input_regs));

	/* Power-on register defaults of the old firmware
	 * (init_app_hook_task() in application/app_hook_task.c).
	 */
	hook_reg_set(CMD_VOLUME, DEFAULT_VOLUME);
	hook_reg_set(REG_ERROR_CODE, 0U);
	hook_reg_set(HEARTBEAT_ENABLE, 1U);
}
