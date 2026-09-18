/*
 * hook_rtc.h - RTC calibration, current time publication and the power
 * schedule (soft standby / STM32 STANDBY).
 *
 * This product has no GPS module: the RTC is calibrated either by the crane
 * host controller over Modbus RTU (holding registers 119/120/121) or by the
 * cloud over the 4G channel (0xA5 command 0x09). Both paths only write the
 * one shot calibration registers; the RTC thread applies them to the RTC and
 * creates the time_manager trust record, then publishes the current Beijing
 * date/time on the input registers 7/8/9.
 */
#ifndef HOOK_RTC_H
#define HOOK_RTC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the RTC / power schedule thread. */
void hook_rtc_start(void);

/* True while a trusted time exists (time_manager RTC trust record valid). */
bool hook_rtc_trust_is_valid(void);

/* Publish the current Beijing date/time on the input registers 7/8/9. */
void hook_rtc_publish_time(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_RTC_H */
