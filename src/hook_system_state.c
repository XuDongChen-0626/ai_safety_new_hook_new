/*
 * hook_system_state.c - single owner for cross-thread product state.
 */

#include "hook_system_state.h"

#include <zephyr/sys/atomic.h>

static atomic_t hook_system_soft_standby = ATOMIC_INIT(0);

void hook_system_state_set_soft_standby(bool standby)
{
	atomic_set(&hook_system_soft_standby, standby ? 1 : 0);
}

bool hook_system_state_is_soft_standby(void)
{
	return atomic_get(&hook_system_soft_standby) != 0;
}
