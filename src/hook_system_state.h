/*
 * hook_system_state.h - product runtime state shared across threads.
 */

#ifndef HOOK_SYSTEM_STATE_H_
#define HOOK_SYSTEM_STATE_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Soft standby is written only by the RTC power-schedule owner. Other threads
 * use the atomic snapshot below; it is equivalent to the previous 0/1 global
 * flag but no longer exposes mutable state through system_def.h.
 */
void hook_system_state_set_soft_standby(bool standby);
bool hook_system_state_is_soft_standby(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_SYSTEM_STATE_H_ */
