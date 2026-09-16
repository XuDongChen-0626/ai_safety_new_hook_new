/*
 * hook_gps.h - GPS NMEA time source (usart3).
 *
 * Ported from application/gps_app.c (config_gps_app / update_gps_app /
 * gps_sync_rtc_once). The RTC power schedule and STANDBY handling live in
 * hook_rtc.c.
 */
#ifndef HOOK_GPS_H
#define HOOK_GPS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the GPS/RTC thread (configures the module, then parses NMEA). */
void hook_gps_start(void);

/* True once a valid NMEA time was written to the RTC (or restored from a
 * trusted RTC/BBRAM record).
 */
bool hook_gps_time_is_trusted(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_GPS_H */
