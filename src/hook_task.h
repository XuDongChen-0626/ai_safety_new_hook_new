/*
 * hook_task.h - application thread creation entry.
 */
#ifndef HOOK_TASK_H
#define HOOK_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* Replaces init_app_hook_task() of the CubeMX firmware. */
void init_app_hook_task(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_TASK_H */
