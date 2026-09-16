/*
 * hook_gps.c - GPS NMEA time source on usart3.
 *
 * Ported from application/gps_app.c:
 *   config_gps_app()  -> hook_gps_config_port()
 *   update_gps_app()  -> hook_gps_update()
 *   gps_sync_rtc_once() -> hook_rtc_set_from_utc() (hook_rtc.c)
 *   process_gps_logic() -> hook_gps_thread()
 */
#include "hook_gps.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "board.h"
#include "hook_health.h"
#include "hook_register.h"
#include "hook_rtc.h"
#include "nmea.h"
#include "osal_zephyr.h"
#include "system_def.h"
#include "uart_manage.h"

LOG_MODULE_REGISTER(hook_gps, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_GPS_CHANNEL "gps"
#define HOOK_GPS_READ_CHUNK 256U
#define HOOK_GPS_LINE_MAX 256U

static bool hook_gps_time_trusted;

bool hook_gps_time_is_trusted(void)
{
	return hook_gps_time_trusted;
}

/*
 * Send the module specific configuration strings, then release GPS_EN.
 * Only the 6N variant of the old firmware is kept active by default
 * (GPS_TYPE_STD in system_def.h selects the command set).
 */
static void hook_gps_config_port(void)
{
	(void)uart_manage_enable_recv_by_name(HOOK_GPS_CHANNEL);
	k_msleep(1000);

#if (GPS_TYPE_STD == WT_RTK_UM982)
	static const char cfg_gga[] = "GPGGA COM1 100\r\n";
	static const char cfg_rmc[] = "GPRMC COM1 1\r\n";
	static const char cfg_save[] = "SAVECONFIG\r\n";

	(void)uart_manage_send_by_name(HOOK_GPS_CHANNEL, (uint8_t *)cfg_gga,
				       (uint16_t)(sizeof(cfg_gga) - 1U));
	k_msleep(10);
	(void)uart_manage_send_by_name(HOOK_GPS_CHANNEL, (uint8_t *)cfg_rmc,
				       (uint16_t)(sizeof(cfg_rmc) - 1U));
	k_msleep(10);
	(void)uart_manage_send_by_name(HOOK_GPS_CHANNEL, (uint8_t *)cfg_save,
				       (uint16_t)(sizeof(cfg_save) - 1U));
	k_msleep(10);
#elif (GPS_TYPE_STD == WT_GPS_UM626N)
	static const char *const cfg_msgs[] = {
		"$CFGMSG,0,0,0\r\n", "$CFGMSG,0,1,0\r\n", "$CFGMSG,0,2,0\r\n",
		"$CFGMSG,0,3,0\r\n", "$CFGMSG,0,4,1\r\n", "$CFGMSG,0,5,0\r\n",
		"$CFGMSG,0,6,0\r\n", "$CFGMSG,0,7,0\r\n",
	};

	for (size_t i = 0U; i < ARRAY_SIZE(cfg_msgs); i++) {
		(void)uart_manage_send_by_name(HOOK_GPS_CHANNEL,
					       (uint8_t *)cfg_msgs[i],
					       (uint16_t)strlen(cfg_msgs[i]));
		k_msleep(10);
	}
#elif (GPS_TYPE_STD == WT_GPS_6N)
	static const char cfg_freq[] = "$PCAS03,1,0,0,0,0,0,0,0*03\r\n";
	static const char cfg_save[] = "$PCAS00*01\r\n";

	/* 1 Hz position/date output, then persist the configuration. */
	(void)uart_manage_send_by_name(HOOK_GPS_CHANNEL, (uint8_t *)cfg_freq,
				       (uint16_t)(sizeof(cfg_freq) - 1U));
	k_msleep(10);
	(void)uart_manage_send_by_name(HOOK_GPS_CHANNEL, (uint8_t *)cfg_save,
				       (uint16_t)(sizeof(cfg_save) - 1U));
	k_msleep(10);
#endif

	GPIO_WRITE(gps_en_gpio, 1);
	k_msleep(3000);
}

static void hook_gps_sync_rtc_once(void)
{
	if (hook_gps_time_trusted) {
		return;
	}

	if (hook_rtc_set_from_utc(g_nmea_gnss.date_year, g_nmea_gnss.date_m,
				  g_nmea_gnss.date_d, g_nmea_gnss.time_h,
				  g_nmea_gnss.time_m,
				  g_nmea_gnss.time_s) != 0) {
		LOG_WRN("GPS time rejected (year=%u %02u-%02u %02u:%02u:%02u)",
			g_nmea_gnss.date_year, g_nmea_gnss.date_m,
			g_nmea_gnss.date_d, g_nmea_gnss.time_h,
			g_nmea_gnss.time_m, g_nmea_gnss.time_s);
		return;
	}

	hook_gps_time_trusted = true;
	hook_health_report(HOOK_HEALTH_EVENT_GPS);

	LOG_INF("GPS time synced: %04u-%02u-%02u %02u:%02u:%02u UTC",
		g_nmea_gnss.date_year, g_nmea_gnss.date_m, g_nmea_gnss.date_d,
		g_nmea_gnss.time_h, g_nmea_gnss.time_m, g_nmea_gnss.time_s);
}

static void hook_gps_update(void)
{
	uart_inferface_t *obj = uart_manage_get_obj_by_name(HOOK_GPS_CHANNEL);
	uint8_t chunk[HOOK_GPS_READ_CHUNK];
	static char line[HOOK_GPS_LINE_MAX];
	static size_t line_len;
	static uint8_t drop_count;
	uint8_t rounds = 0U;

	if (obj == NULL) {
		return;
	}

	/* Drain the ring buffer and reassemble complete NMEA sentences: the
	 * parser expects one sentence per call.
	 */
	while (rounds++ < 8U) {
		lwrb_sz_t available = lwrb_get_full(&obj->process_ring_buffer);
		lwrb_sz_t to_read;
		lwrb_sz_t read_size;

		if (available == 0U) {
			break;
		}

		to_read = (available > sizeof(chunk)) ? sizeof(chunk) : available;
		read_size = lwrb_read(&obj->process_ring_buffer, chunk, to_read);
		if (read_size == 0U) {
			break;
		}

		for (lwrb_sz_t i = 0U; i < read_size; i++) {
			char c = (char)chunk[i];

			if ((c == '\n') || (c == '\r')) {
				if (line_len > 0U) {
					int parse_result;

					line[line_len] = '\0';
					parse_result = nmea_parse((const uint8_t *)line,
								  (uint16_t)line_len);
					if ((parse_result != NMEA_OK) &&
					    (parse_result != NMEA_ERR_NO_FIX)) {
						LOG_DBG_RATELIMIT("NMEA parse error %d",
								  parse_result);
					} else if ((g_nmea_gnss.date_year >= 2024U) &&
						   ((g_nmea_gnss.time_h != 0U) ||
						    (g_nmea_gnss.time_m != 0U) ||
						    (g_nmea_gnss.time_s != 0U))) {
						hook_gps_sync_rtc_once();
					}
				}
				line_len = 0U;
				continue;
			}

			if (line_len < (HOOK_GPS_LINE_MAX - 1U)) {
				line[line_len++] = c;
			} else if (drop_count < 5U) {
				drop_count++;
				LOG_WRN("NMEA sentence longer than %u bytes, dropped",
					HOOK_GPS_LINE_MAX);
				line_len = 0U;
			} else {
				line_len = 0U;
			}
		}
	}
}

static void hook_gps_thread(void *a, void *b, void *c)
{
	static uint32_t last_schedule_ms;

	(void)a;
	(void)b;
	(void)c;

	hook_gps_config_port();
	hook_rtc_power_init();

	/* A trusted RTC (GPS sync or host calibration done earlier) is enough
	 * to run the power schedule, exactly like s_gps_synced in the old
	 * firmware.
	 */
	if (hook_rtc_trust_is_valid()) {
		hook_gps_time_trusted = true;
		LOG_INF("RTC trust record valid, power schedule enabled");
	} else {
		LOG_WRN("no trusted RTC time yet, waiting for GPS");
	}

	for (;;) {
		hook_health_task_alive(HOOK_TASK_ALIVE_GPS);
		hook_gps_update();

		if (hook_get_time_ms() - last_schedule_ms >= 1000U) {
			last_schedule_ms = hook_get_time_ms();
			hook_rtc_power_schedule_check();
		}

		k_msleep(100);
	}
}

K_THREAD_STACK_DEFINE(hook_gps_stack, 4096);
static struct k_thread hook_gps_thread_obj;

void hook_gps_start(void)
{
	k_thread_create(&hook_gps_thread_obj, hook_gps_stack,
			K_THREAD_STACK_SIZEOF(hook_gps_stack),
			hook_gps_thread, NULL, NULL, NULL,
			OSAL_PRIO_BELOW_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_gps_thread_obj, "GPSStandby");
}
