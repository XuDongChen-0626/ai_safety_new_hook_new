/*
 * hook_4g_ntp.h - trusted time from the 4G module's NTP client.
 */
#ifndef HOOK_4G_NTP_H
#define HOOK_4G_NTP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_HOOK_4G_NTP_ENABLE)

void hook_4g_ntp_start(void);

/*
 * Prevent a new AT+CCLK exchange and wait for one already in progress.
 * HTTP OTA uses this while the module is switched to HTTPD mode.
 */
void hook_4g_ntp_set_paused(bool paused);

#else

static inline void hook_4g_ntp_start(void)
{
}

static inline void hook_4g_ntp_set_paused(bool paused)
{
	(void)paused;
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* HOOK_4G_NTP_H */
