/*
 * hook_work_mode.h - aggregated work mode for the input register table.
 *
 * Ported from the old app_sys_supervisor.c WorkModeTask: it derives
 * STATUS_WORK_MODE (input register 106) from the error code, the battery level
 * and the soft-standby flag. The heart LED that used to live next to it is now
 * driven by the system_health_service LED backend from the health events.
 */
#ifndef HOOK_WORK_MODE_H
#define HOOK_WORK_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

void hook_work_mode_start(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_WORK_MODE_H */