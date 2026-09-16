/*
 * hook_rtc.c - RTC power schedule, host calibration and STM32 STANDBY.
 *
 * Time convention: the internal RTC stores UTC. The power schedule
 * registers (111/112) and the RTC_TIME register (114) use Beijing
 * wall-clock time (UTC+8), exactly like the old firmware. The alarm used
 * for the hard standby wake-up is programmed in UTC.
 */
#include "hook_rtc.h"

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/timeutil.h>

#include "board.h"
#include "hook_health.h"
#include "hook_gps.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "pwr_stm32.h"
#include "system_def.h"

#if defined(CONFIG_TIME_RTC_TRUST_STORE)
#include "rtc_trust_store.h"
#endif
#if defined(CONFIG_TIME_SOURCE_RTC)
#include "time_source_rtc.h"
#endif

LOG_MODULE_REGISTER(hook_rtc, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_RTC_DEV DEVICE_DT_GET(DT_NODELABEL(rtc))

/* Alarm B is id 1 in the Zephyr STM32 RTC driver (id 0 = alarm A). */
#define HOOK_RTC_ALARM_B_ID 1U

#define BEIJING_UTC_OFFSET_HOURS 8
#define BEIJING_UTC_OFFSET_SECONDS (BEIJING_UTC_OFFSET_HOURS * 3600)

#if defined(CONFIG_TIME_RTC_MIN_VALID_UNIX_TIME)
#define HOOK_RTC_MIN_VALID_UNIX_S ((int64_t)CONFIG_TIME_RTC_MIN_VALID_UNIX_TIME)
#else
#define HOOK_RTC_MIN_VALID_UNIX_S ((int64_t)1704067200)
#endif

bool hook_rtc_trust_is_valid(void)
{
#if defined(CONFIG_TIME_RTC_TRUST_STORE)
	return rtc_trust_store_is_valid();
#else
	return true;
#endif
}

static uint8_t hook_bj_hour_to_utc(uint8_t bj_hour)
{
	return (uint8_t)((bj_hour + 24U - BEIJING_UTC_OFFSET_HOURS) % 24U);
}

static int hook_rtc_get_unix_s(int64_t *unix_s)
{
	struct rtc_time rtc_tm;
	struct tm tm_utc;

	if ((unix_s == NULL) || (rtc_get_time(HOOK_RTC_DEV, &rtc_tm) != 0)) {
		return -EIO;
	}

	tm_utc = (struct tm) {
		.tm_sec = rtc_tm.tm_sec,
		.tm_min = rtc_tm.tm_min,
		.tm_hour = rtc_tm.tm_hour,
		.tm_mday = rtc_tm.tm_mday,
		.tm_mon = rtc_tm.tm_mon,
		.tm_year = rtc_tm.tm_year,
		.tm_isdst = 0,
	};
	*unix_s = timeutil_timegm64(&tm_utc);

	return 0;
}

static int hook_rtc_set_unix_s(int64_t unix_s)
{
	time_t time_s = (time_t)unix_s;
	struct tm tm_utc;
	struct rtc_time rtc_tm;

	if ((unix_s < HOOK_RTC_MIN_VALID_UNIX_S) ||
	    (gmtime_r(&time_s, &tm_utc) == NULL)) {
		return -EINVAL;
	}

	rtc_tm = (struct rtc_time) {
		.tm_sec = tm_utc.tm_sec,
		.tm_min = tm_utc.tm_min,
		.tm_hour = tm_utc.tm_hour,
		.tm_mday = tm_utc.tm_mday,
		.tm_mon = tm_utc.tm_mon,
		.tm_year = tm_utc.tm_year,
		.tm_wday = tm_utc.tm_wday,
		.tm_yday = tm_utc.tm_yday,
		.tm_isdst = 0,
		.tm_nsec = 0,
	};

	return rtc_set_time(HOOK_RTC_DEV, &rtc_tm);
}

static void hook_rtc_refresh_trust(int64_t unix_s)
{
#if defined(CONFIG_TIME_RTC_TRUST_STORE) && defined(CONFIG_TIME_SOURCE_RTC)
	if (unix_s < HOOK_RTC_MIN_VALID_UNIX_S) {
		return;
	}

	if (rtc_trust_store_mark_valid(RTC_TRUST_SOURCE_GPS, unix_s) == 0) {
		(void)time_source_rtc_refresh();
	}
#else
	ARG_UNUSED(unix_s);
#endif
}

void hook_rtc_publish_time(void)
{
	int64_t unix_s;
	struct tm bj;
	time_t bj_time;

	if (hook_rtc_get_unix_s(&unix_s) != 0) {
		return;
	}

	bj_time = (time_t)(unix_s + BEIJING_UTC_OFFSET_SECONDS);
	if (gmtime_r(&bj_time, &bj) == NULL) {
		return;
	}

	hook_reg_set(RTC_TIME,
		     (uint16_t)(((uint8_t)bj.tm_hour << 8) | (uint8_t)bj.tm_min));
}

int hook_rtc_set_from_utc(uint16_t year, uint8_t month, uint8_t day,
			  uint8_t hour, uint8_t minute, uint8_t second)
{
	struct tm tm_utc = {
		.tm_year = (int)year - 1900,
		.tm_mon = (int)month - 1,
		.tm_mday = (int)day,
		.tm_hour = (int)hour,
		.tm_min = (int)minute,
		.tm_sec = (int)second,
		.tm_isdst = 0,
	};
	int64_t unix_s;
	int ret;

	if ((year < 2000U) || (year > 2099U) || (month < 1U) || (month > 12U) ||
	    (day < 1U) || (day > 31U) || (hour > 23U) || (minute > 59U) ||
	    (second > 59U)) {
		return -EINVAL;
	}

	unix_s = timeutil_timegm64(&tm_utc);
	ret = hook_rtc_set_unix_s(unix_s);
	if (ret != 0) {
		return ret;
	}

	hook_rtc_refresh_trust(unix_s);
	hook_rtc_publish_time();

	return 0;
}

void hook_rtc_power_init(void)
{
	uint16_t off_time = hook_reg_get(STATUS_POWER_OFF_TIME);

	if (pwr_wakeup_from_standby()) {
		LOG_INF("[PWR] wake-up from hard standby (RTC alarm B)");
		pwr_clear_standby_flag();
	} else {
		LOG_INF("[PWR] cold start");
	}

	if (off_time == 0U) {
		hook_reg_set(STATUS_POWER_OFF_TIME, POWER_OFF_DEFAULT);
		hook_reg_set(STATUS_POWER_ON_TIME, POWER_ON_DEFAULT);
		LOG_INF("[PWR] default schedule: OFF=%02u:%02u ON=%02u:%02u",
			POWER_OFF_DEFAULT >> 8, POWER_OFF_DEFAULT & 0xFFU,
			POWER_ON_DEFAULT >> 8, POWER_ON_DEFAULT & 0xFFU);
	}

	/* Soft standby (relay off) is enabled by default, hard standby is
	 * opt-in from the host through register 115.
	 */
	hook_reg_set(SOFT_STANDBY_ENABLE, 1U);

	hook_rtc_publish_time();
}

/* beijing_h/beijing_m are Beijing time; the RTC counter is UTC. */
static void hook_rtc_set_alarm_b(uint8_t beijing_h, uint8_t beijing_m)
{
	struct rtc_time alarm = {0};
	uint16_t mask = RTC_ALARM_TIME_MASK_HOUR |
			RTC_ALARM_TIME_MASK_MINUTE |
			RTC_ALARM_TIME_MASK_SECOND;

	alarm.tm_hour = hook_bj_hour_to_utc(beijing_h);
	alarm.tm_min = beijing_m;
	alarm.tm_sec = 0;

	if (rtc_alarm_set_time(HOOK_RTC_DEV, HOOK_RTC_ALARM_B_ID, mask,
			       &alarm) != 0) {
		LOG_ERR("[RTC] alarm B could not be armed");
		return;
	}

	LOG_INF("[RTC] standby alarm B armed for %02u:%02u (Beijing)",
		beijing_h, beijing_m);
}

static void hook_rtc_enter_standby(void)
{
#if defined(CONFIG_HOOK_HARD_STANDBY_ENABLE)
	if (!hook_rtc_trust_is_valid()) {
		LOG_ERR("[PWR-ERR] RTC not trusted, standby aborted");
		return;
	}

	LOG_INF("[PWR] entering STM32 hard standby");
	GPIO_WRITE(gps_en_gpio, 0);
	k_msleep(200);

	pwr_clear_rtc_alarm_b_flag();
	pwr_clear_wakeup_flags();
	pwr_clear_standby_flag();
	pwr_enter_standby();
#else
	LOG_WRN("[PWR] hard standby disabled by configuration");
#endif
}

static bool hook_rtc_time_should_sleep(uint16_t now_hhmm, uint16_t off_hhmm,
				       uint16_t on_hhmm)
{
	if (off_hhmm < on_hhmm) {
		return (now_hhmm >= off_hhmm) && (now_hhmm < on_hhmm);
	}

	if (off_hhmm > on_hhmm) {
		return (now_hhmm >= off_hhmm) || (now_hhmm < on_hhmm);
	}

	return false;
}

void hook_rtc_power_schedule_check(void)
{
	int64_t unix_s;
	struct tm bj;
	time_t bj_time;
	uint16_t now_hhmm;
	uint16_t off_hhmm;
	uint16_t on_hhmm;
	uint16_t soft_enable;
	uint16_t hard_enable;
	static uint16_t last_trigger_hhmm = 0xFFFFU;

	if (hook_rtc_get_unix_s(&unix_s) != 0) {
		return;
	}

	hook_rtc_publish_time();

	if (!hook_gps_time_is_trusted() && !hook_rtc_trust_is_valid()) {
		/* Without any trusted time the schedule must not be evaluated. */
		return;
	}

	bj_time = (time_t)(unix_s + BEIJING_UTC_OFFSET_SECONDS);
	if (gmtime_r(&bj_time, &bj) == NULL) {
		return;
	}

	now_hhmm = (uint16_t)(((uint8_t)bj.tm_hour << 8) | (uint8_t)bj.tm_min);
	off_hhmm = hook_reg_get(STATUS_POWER_OFF_TIME);
	on_hhmm = hook_reg_get(STATUS_POWER_ON_TIME);
	soft_enable = hook_reg_get(SOFT_STANDBY_ENABLE);
	hard_enable = hook_reg_get(STM32_STANDBY_ENABLE);

	/* Hard standby on the exact power-off minute (once per minute). */
	if ((now_hhmm == off_hhmm) && (hard_enable == 1U) &&
	    (now_hhmm != last_trigger_hhmm)) {
		last_trigger_hhmm = now_hhmm;

		hook_reg_set(CMD_LED_SWITCH, 0U);
		hook_reg_set(CMD_BUZZER_7M, 0U);
		hook_reg_set(CMD_BUZZER_3M, 0U);
		hook_reg_set(STATUS_LED_SWITCH, 0U);
		hook_reg_set(STATUS_BUZZER, 0U);

		hook_rtc_set_alarm_b((uint8_t)(on_hhmm >> 8),
				     (uint8_t)(on_hhmm & 0xFFU));
		hook_rtc_enter_standby();
		return;
	}

	if (soft_enable == 1U) {
		bool should_sleep =
			hook_rtc_time_should_sleep(now_hhmm, off_hhmm, on_hhmm);

		if (should_sleep && (is_soft_standby == 0U)) {
			is_soft_standby = 1U;

			hook_reg_set(CMD_LED_SWITCH, 0U);
			hook_reg_set(CMD_BUZZER_7M, 0U);
			hook_reg_set(CMD_BUZZER_3M, 0U);
			hook_reg_set(STATUS_LED_SWITCH, 0U);
			hook_reg_set(STATUS_BUZZER, 0U);
			GPIO_WRITE(relay_en_gpio, 0);
			GPIO_WRITE(pwr_led_gpio, 0);

			hook_reg_clear_error_bits(ERR_HEARTBEAT_TIMEOUT |
						  ERR_LED_OFFLINE);
			hook_health_sleep_update(true);

			LOG_INF("[PWR] soft standby entered (relay off)");
		} else if (!should_sleep && (is_soft_standby == 1U)) {
			is_soft_standby = 0U;

			GPIO_WRITE(relay_en_gpio, 1);
			/* PWD_LED is re-driven by app/hook_alarm (it mirrors
			 * the Modbus flash-light command register).
			 */
			hook_health_sleep_update(false);

			LOG_INF("[PWR] soft standby left (relay on)");
		}
	} else if (is_soft_standby == 1U) {
		is_soft_standby = 0U;

		GPIO_WRITE(relay_en_gpio, 1);
		hook_health_sleep_update(false);

		LOG_INF("[PWR] soft standby disabled by host, relay on");
	}
}
