/*
 * osal_zephyr.h - application thread priority names.
 *
 * The numerical mapping keeps the priorities used by the former
 * FreeRTOS/CMSIS-OS application while expressing them as Zephyr preemptive
 * priorities.
 */
#ifndef OSAL_ZEPHYR_H
#define OSAL_ZEPHYR_H

#define OSAL_PRIO_ABOVE_NORMAL   1
#define OSAL_PRIO_NORMAL1        2
#define OSAL_PRIO_NORMAL         3
#define OSAL_PRIO_BELOW_NORMAL   4
#define OSAL_PRIO_BELOW_NORMAL1  5
#define OSAL_PRIO_LOW            6

#endif /* OSAL_ZEPHYR_H */
