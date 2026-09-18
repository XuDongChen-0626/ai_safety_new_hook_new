/*
 * hook_health.h - hook field events and the product watchdog policy.
 *
 * The hook controller keeps the old firmware's "all tasks check in"
 * watchdog contract: every business thread sets its own alive bit once per
 * cycle, and the health thread only reports HOOK_HEALTH_EVENT_TASK_ALIVE
 * when a full round of check-ins arrived. When that event goes offline the
 * system health service stops feeding the IWDG, which resets the device.
 *
 * Field events (BMS / alarm module / 4G / host heartbeat) are reported for
 * logging and for the host error-code register; they never reboot a safety
 * controller. The RTC has no field device to talk to (no GPS on this
 * product), so it is only covered by the task check-in contract below.
 */
#ifndef HOOK_HEALTH_H
#define HOOK_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hook_health_event {
	HOOK_HEALTH_EVENT_TASK_ALIVE = 1,
	HOOK_HEALTH_EVENT_BMS = 2,
	HOOK_HEALTH_EVENT_ALARM = 3,
	HOOK_HEALTH_EVENT_HOST_HEARTBEAT = 5,
};

#if defined(CONFIG_SYS_HEALTH_SERVICE)

/*
 * Report that the calling thread is alive. A check-in means "the thread is
 * running", NOT "the thread had work to do", so every thread must call this
 * unconditionally at the top of its loop - also while its work is parked in
 * soft standby. Mode dependent pauses are expressed with
 * hook_health_sleep_update() / hook_health_ota_gate() instead of silently
 * skipping the check-in, otherwise a normal standby looks like a hung thread
 * and (with the watchdog feed backend enabled) would reset the controller.
 */
void hook_health_task_alive(uint32_t mask);

/*
 * Report a successful interaction with a field device. The polling modules
 * only report successes and update measured values; the online/offline
 * decision and everything that follows from it (error-code bits, logs,
 * action policy) belongs to the health service and its event callbacks.
 */
void hook_health_report(uint16_t event);

/* Disable/enable the field events while the controller is in soft standby. */
void hook_health_sleep_update(bool sleeping);

/*
 * Suspend/resume the task check-in supervision. Used by the OTA service:
 * flash sector erase and the Ymodem transfer can keep a business thread busy
 * for longer than the check-in window, which must not trip the watchdog.
 */
void hook_health_ota_gate(bool active);

/* Start the check-in supervision thread. */
void hook_health_task_start(void);

#else /* !CONFIG_SYS_HEALTH_SERVICE */

static inline void hook_health_task_alive(uint32_t mask)
{
	(void)mask;
}

static inline void hook_health_report(uint16_t event)
{
	(void)event;
}

static inline void hook_health_sleep_update(bool sleeping)
{
	(void)sleeping;
}

static inline void hook_health_ota_gate(bool active)
{
	(void)active;
}

static inline void hook_health_task_start(void)
{
}

#endif /* CONFIG_SYS_HEALTH_SERVICE */

#ifdef __cplusplus
}
#endif

#endif /* HOOK_HEALTH_H */
