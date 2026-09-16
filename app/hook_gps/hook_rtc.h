/*
 * hook_rtc.h - RTC power schedule and STM32 STANDBY handling.
 *
 * Ported from the power-management half of application/gps_app.c
 * (rtc_power_init / rtc_power_schedule_check / set_alarm_b / enter_standby).
 */
#ifndef HOOK_RTC_H
#define HOOK_RTC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Publish the current Beijing time in register 114 for the host/cloud. */
void hook_rtc_publish_time(void);

/* RTC trust guard used by the standby path (GPS sync or host calibration). */
bool hook_rtc_trust_is_valid(void);

/*
 * Write the RTC from a UTC date/time (GPS) and create the trust record.
 * Returns 0 when the time was accepted.
 */
int hook_rtc_set_from_utc(uint16_t year, uint8_t month, uint8_t day,
			  uint8_t hour, uint8_t minute, uint8_t second);

/* Seed the power schedule defaults and detect a standby wake-up. */
void hook_rtc_power_init(void);

/* One schedule evaluation step (called once per second by the GPS thread). */
void hook_rtc_power_schedule_check(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_RTC_H */
