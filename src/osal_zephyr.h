/*
 * osal_zephyr.h - thin Zephyr port helpers used by the migrated
 * application code (formerly FreeRTOS / CMSIS-OS v2 based).
 *
 * The original code used FreeRTOS task notifications heavily
 * (xTaskNotify / ulTaskNotifyTake). Zephyr has no direct equivalent,
 * so a small "signal + value" object is provided here.
 */
#ifndef OSAL_ZEPHYR_H
#define OSAL_ZEPHYR_H

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Task notification emulation (FreeRTOS xTaskNotify / ulTaskNotifyTake) */
/* ------------------------------------------------------------------ */

typedef struct {
	struct k_sem sem;
	atomic_t value;
} osal_notify_t;

#define OSAL_NOTIFY_TIMEOUT_INFINITE 0xFFFFFFFFUL

static inline void osal_notify_init(osal_notify_t *n)
{
	k_sem_init(&n->sem, 0, 1);
	atomic_set(&n->value, 0);
}

/* Signal a notification. Safe to call from ISR / timer callbacks. */
static inline void osal_notify_signal(osal_notify_t *n, uint32_t value)
{
	if (n == NULL) {
		return;
	}
	atomic_set(&n->value, (atomic_val_t)value);
	k_sem_give(&n->sem);
}

static inline uint32_t osal_notify_wait(osal_notify_t *n, uint32_t timeout_ms)
{
	int ret;

	if (n == NULL) {
		return 0U;
	}

	if (timeout_ms == OSAL_NOTIFY_TIMEOUT_INFINITE) {
		ret = k_sem_take(&n->sem, K_FOREVER);
	} else {
		ret = k_sem_take(&n->sem, K_MSEC(timeout_ms));
	}

	if (ret != 0) {
		return 0U;
	}

	return (uint32_t)atomic_get(&n->value);
}

static inline uint32_t osal_notify_wait_forever(osal_notify_t *n)
{
	return osal_notify_wait(n, OSAL_NOTIFY_TIMEOUT_INFINITE);
}

/* ------------------------------------------------------------------ */
/* Thread -> notify registry                                          */
/*                                                                     */
/* ModbusQuery()/ModbusQueryV2() need to notify the *calling* thread.  */
/* Each thread that calls them registers its own osal_notify_t once at */
/* thread start (see app_* threads).                                   */
/* ------------------------------------------------------------------ */

#define OSAL_MAX_NOTIFY_SLOTS 8U

typedef struct {
	struct k_thread *thread;
	osal_notify_t *notify;
} osal_notify_slot_t;

extern osal_notify_slot_t g_osal_notify_slots[OSAL_MAX_NOTIFY_SLOTS];

void osal_notify_register_current(osal_notify_t *n);
void osal_notify_unregister_current(void);
osal_notify_t *osal_notify_current(void);

/* ------------------------------------------------------------------ */
/* Priority mapping                                                    */
/*                                                                     */
/* Zephyr preemptive priorities: 0 = highest, larger = lower.          */
/* The mapping below matches the original osPriority values:           */
/*   osPriorityAboveNormal -> OSAL_PRIO_ABOVE_NORMAL (highest app)     */
/*   osPriorityLow         -> OSAL_PRIO_LOW                            */
/* ------------------------------------------------------------------ */

#define OSAL_PRIO_ABOVE_NORMAL   1
#define OSAL_PRIO_NORMAL1        2
#define OSAL_PRIO_NORMAL         3
#define OSAL_PRIO_BELOW_NORMAL   4
#define OSAL_PRIO_BELOW_NORMAL1  5
#define OSAL_PRIO_LOW            6

#ifdef __cplusplus
}
#endif

#endif /* OSAL_ZEPHYR_H */
