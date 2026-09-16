#include "osal_zephyr.h"

osal_notify_slot_t g_osal_notify_slots[OSAL_MAX_NOTIFY_SLOTS];
unsigned int g_osal_critical_key;

void osal_notify_register_current(osal_notify_t *n)
{
	struct k_thread *me = k_current_get();
	unsigned int key;
	int i;

	if (n == NULL) {
		return;
	}

	key = irq_lock();
	for (i = 0; i < OSAL_MAX_NOTIFY_SLOTS; i++) {
		if (g_osal_notify_slots[i].thread == NULL) {
			g_osal_notify_slots[i].thread = me;
			g_osal_notify_slots[i].notify = n;
			break;
		}
	}
	irq_unlock(key);
}

void osal_notify_unregister_current(void)
{
	struct k_thread *me = k_current_get();
	unsigned int key;
	int i;

	key = irq_lock();
	for (i = 0; i < OSAL_MAX_NOTIFY_SLOTS; i++) {
		if (g_osal_notify_slots[i].thread == me) {
			g_osal_notify_slots[i].thread = NULL;
			g_osal_notify_slots[i].notify = NULL;
			break;
		}
	}
	irq_unlock(key);
}

osal_notify_t *osal_notify_current(void)
{
	struct k_thread *me = k_current_get();
	int i;

	for (i = 0; i < OSAL_MAX_NOTIFY_SLOTS; i++) {
		if (g_osal_notify_slots[i].thread == me) {
			return g_osal_notify_slots[i].notify;
		}
	}

	return NULL;
}
