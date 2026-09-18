/*
 * hook_rtc.c - RTC calibration, time publication and the power schedule.
 *
 * Ported from the power-management half of the old application/gps_app.c
 * (rtc_power_init / rtc_power_schedule_check / set_alarm_b / enter_standby)
 * and aligned with craner_ai_safety_car, which drives the same logic from
 * Modbus registers after its GPS module was removed.
 *
 * Time convention: the internal RTC stores UTC, matching
 * module/time_manager_service (CONFIG_TIME_RTC_BACKEND_INSIDE_RTC). The
 * calibration registers (holding 119/120/121) and the display registers
 * (input 7/8/9) use Beijing wall-clock time (UTC+8); the alarm that wakes the
 * MCU from STANDBY is programmed in UTC.
 */
#include "hook_rtc.h"

#include <time.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/timeutil.h>

#include "board.h"
#include "hook_health.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "offline_log_service.h"
#include "osal_zephyr.h"
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
#define HOOK_RTC_MIN_VALID_UNIX_S ((int64_t)1704067200) /* 2024-01-01 UTC */
#endif

/* A calibration command that is only partially written is dropped after this
 * long, so a stale half command can never be completed much later.
 */
#define HOOK_RTC_CAL_TIMEOUT_MS (60U * 1000U)

/* The schedule only has a one minute resolution; 5 s of jitter is plenty. */
#define HOOK_RTC_SCHEDULE_PERIOD_MS 5000U

bool hook_rtc_trust_is_valid(void)
{
#if defined(CONFIG_TIME_RTC_TRUST_STORE)
	return rtc_trust_store_is_valid();
#else
	return true;
#endif
}

static uint8_t hook_rtc_bj_hour_to_utc(uint8_t bj_hour)
{
	return (uint8_t)((bj_hour + 24U - BEIJING_UTC_OFFSET_HOURS) % 24U);
}

static uint8_t hook_rtc_utc_hour_to_bj(uint8_t utc_hour)
{
	return (uint8_t)((utc_hour + BEIJING_UTC_OFFSET_HOURS) % 24U);
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

static int hook_rtc_get_beijing_tm(struct tm *bj)
{
	int64_t unix_s = 0;
	time_t bj_time;

	if ((bj == NULL) || (hook_rtc_get_unix_s(&unix_s) != 0)) {
		return -EIO;
	}

	bj_time = (time_t)(unix_s + BEIJING_UTC_OFFSET_SECONDS);
	if (gmtime_r(&bj_time, bj) == NULL) {
		return -EINVAL;
	}

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

static void hook_rtc_log_internal_time(void)
{
	struct rtc_time tm = {0};

	if (rtc_get_time(HOOK_RTC_DEV, &tm) != 0) {
		LOG_ERR("[RTC] rtc_get_time failed");
		return;
	}

	LOG_INF("[RTC] internal time %04d-%02d-%02d %02d:%02d:%02d UTC "
		"(Beijing %02d:%02d:%02d)",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		hook_rtc_utc_hour_to_bj((uint8_t)tm.tm_hour), tm.tm_min, tm.tm_sec);
}

/* Refresh time_manager with the RTC value just written and (re)create the
 * trust record; the host/cloud calibration is a "manual" trust source.
 */
static void hook_rtc_refresh_trust(int64_t unix_s)
{
#if defined(CONFIG_TIME_RTC_TRUST_STORE) && defined(CONFIG_TIME_SOURCE_RTC)
	if (unix_s < HOOK_RTC_MIN_VALID_UNIX_S) {
		return;
	}

	if (rtc_trust_store_mark_valid(RTC_TRUST_SOURCE_MANUAL, unix_s) == 0) {
		(void)time_source_rtc_refresh();
	}
#else
	ARG_UNUSED(unix_s);
#endif
}

void hook_rtc_publish_time(void)
{
	struct tm bj;

	if (hook_rtc_get_beijing_tm(&bj) != 0) {
		return;
	}

	hook_reg_input_set(INPUT_RTC_YEAR, (uint16_t)(bj.tm_year + 1900));
	hook_reg_input_set(INPUT_RTC_MONTH_DAY,
			   (uint16_t)(((bj.tm_mon + 1) << 8) | bj.tm_mday));
	hook_reg_input_set(INPUT_RTC_TIME_HHMM,
			   (uint16_t)((bj.tm_hour << 8) | bj.tm_min));
}

/* --- one shot calibration command (holding 119/120/121) ------------- */

static bool hook_rtc_calendar_date_valid(uint16_t year, uint16_t month_day)
{
	static const uint8_t days_in_month[] = {
		31U, 28U, 31U, 30U, 31U, 30U,
		31U, 31U, 30U, 31U, 30U, 31U,
	};
	uint8_t month = (uint8_t)(month_day >> 8);
	uint8_t day = (uint8_t)(month_day & 0xFFU);
	bool leap;
	uint8_t max_day;

	if ((year < 2000U) || (year > 2099U) || (month < 1U) || (month > 12U)) {
		return false;
	}

	leap = ((year % 4U) == 0U) &&
	       (((year % 100U) != 0U) || ((year % 400U) == 0U));
	max_day = days_in_month[month - 1U];
	if ((month == 2U) && leap) {
		max_day++;
	}

	return (day >= 1U) && (day <= max_day);
}

static bool hook_rtc_hhmm_valid(uint16_t hhmm)
{
	return (((hhmm >> 8) & 0xFFU) <= 23U) && ((hhmm & 0xFFU) <= 59U);
}

/* Convert Beijing wall-clock fields (seconds forced to 0) to a UTC unix time. */
static bool hook_rtc_fields_to_unix(uint16_t year, uint16_t month_day,
				    uint16_t hhmm, int64_t *unix_s)
{
	struct tm bj_tm;
	int64_t bj_as_utc;

	if ((unix_s == NULL) || !hook_rtc_calendar_date_valid(year, month_day) ||
	    !hook_rtc_hhmm_valid(hhmm)) {
		return false;
	}

	bj_tm = (struct tm) {
		.tm_year = (int)year - 1900,
		.tm_mon = (int)((month_day >> 8) & 0xFFU) - 1,
		.tm_mday = (int)(month_day & 0xFFU),
		.tm_hour = (int)((hhmm >> 8) & 0xFFU),
		.tm_min = (int)(hhmm & 0xFFU),
		.tm_sec = 0,
		.tm_isdst = 0,
	};

	/* timeutil_timegm64() reads the fields as UTC; Beijing is UTC+8, so the
	 * real UTC instant is eight hours earlier.
	 */
	bj_as_utc = timeutil_timegm64(&bj_tm);
	*unix_s = bj_as_utc - BEIJING_UTC_OFFSET_SECONDS;

	return *unix_s >= HOOK_RTC_MIN_VALID_UNIX_S;
}

static int hook_rtc_apply_calibration(uint16_t year, uint16_t month_day,
				      uint16_t hhmm)
{
	int64_t unix_s = 0;

	if (!hook_rtc_fields_to_unix(year, month_day, hhmm, &unix_s)) {
		LOG_WRN("[RTC] calibration rejected: year=%u md=%04X hhmm=%04X",
			year, month_day, hhmm);
		return -EINVAL;
	}

	if (hook_rtc_set_unix_s(unix_s) != 0) {
		LOG_ERR("[RTC] rtc_set_time failed");
		return -EIO;
	}

	LOG_INF("[RTC] time synced from host (Beijing) %04u-%02u-%02u %02u:%02u:00",
		year, (month_day >> 8) & 0xFFU, month_day & 0xFFU,
		(hhmm >> 8) & 0xFFU, hhmm & 0xFFU);

	hook_rtc_refresh_trust(unix_s);
	hook_rtc_publish_time();

	(void)offline_log_event(OFFLINE_LOG_CAT_POWER, OFFLINE_LOG_LEVEL_INF,
				"RTC calibrated from host: %04u-%02u-%02u %02u:%02u:00 "
				"(Beijing), trust record rebuilt",
				year, (month_day >> 8) & 0xFFU, month_day & 0xFFU,
				(hhmm >> 8) & 0xFFU, hhmm & 0xFFU);

	return 0;
}

/*
 * Consume the calibration registers. The host may write the three registers
 * one by one (three FC06 frames) or as one block (one FC16 frame), so the
 * command is executed only when all three left their "not written" default
 * and the resulting date/time is valid.
 */
static void hook_rtc_calibration_check(void)
{
	uint16_t year = hook_reg_get(RTC_CAL_YEAR);
	uint16_t month_day = hook_reg_get(RTC_CAL_MONTH_DAY);
	uint16_t hhmm = hook_reg_get(RTC_CAL_HHMM);
	static uint16_t prev_year = RTC_CAL_YEAR_DEFAULT;
	static uint16_t prev_month_day = RTC_CAL_MONTH_DAY_DEFAULT;
	static uint16_t prev_hhmm = RTC_CAL_HHMM_DEFAULT;
	static uint32_t pending_since_ms;
	bool pending = (year != RTC_CAL_YEAR_DEFAULT) ||
		       (month_day != RTC_CAL_MONTH_DAY_DEFAULT) ||
		       (hhmm != RTC_CAL_HHMM_DEFAULT);
	bool changed = (year != prev_year) || (month_day != prev_month_day) ||
		       (hhmm != prev_hhmm);

	if (pending && hook_rtc_calendar_date_valid(year, month_day) &&
	    hook_rtc_hhmm_valid(hhmm) &&
	    ((year != RTC_CAL_YEAR_DEFAULT) &&
	     (month_day != RTC_CAL_MONTH_DAY_DEFAULT) &&
	     (hhmm != RTC_CAL_HHMM_DEFAULT))) {
		if (hook_rtc_apply_calibration(year, month_day, hhmm) == 0) {
			prev_year = RTC_CAL_YEAR_DEFAULT;
			prev_month_day = RTC_CAL_MONTH_DAY_DEFAULT;
			prev_hhmm = RTC_CAL_HHMM_DEFAULT;
			pending_since_ms = 0U;
		}

		/* Consume the command registers even when the value was rejected
		 * (invalid dates are reported once and do not stay armed).
		 */
		hook_reg_set(RTC_CAL_YEAR, RTC_CAL_YEAR_DEFAULT);
		hook_reg_set(RTC_CAL_MONTH_DAY, RTC_CAL_MONTH_DAY_DEFAULT);
		hook_reg_set(RTC_CAL_HHMM, RTC_CAL_HHMM_DEFAULT);
		return;
	}

	if (!pending) {
		pending_since_ms = 0U;
		prev_year = RTC_CAL_YEAR_DEFAULT;
		prev_month_day = RTC_CAL_MONTH_DAY_DEFAULT;
		prev_hhmm = RTC_CAL_HHMM_DEFAULT;
		return;
	}

	if (changed || (pending_since_ms == 0U)) {
		pending_since_ms = k_uptime_get_32();
	}
	prev_year = year;
	prev_month_day = month_day;
	prev_hhmm = hhmm;

	if ((k_uptime_get_32() - pending_since_ms) >= HOOK_RTC_CAL_TIMEOUT_MS) {
		LOG_WRN("[RTC] incomplete calibration abandoned after %u s",
			HOOK_RTC_CAL_TIMEOUT_MS / 1000U);
		hook_reg_set(RTC_CAL_YEAR, RTC_CAL_YEAR_DEFAULT);
		hook_reg_set(RTC_CAL_MONTH_DAY, RTC_CAL_MONTH_DAY_DEFAULT);
		hook_reg_set(RTC_CAL_HHMM, RTC_CAL_HHMM_DEFAULT);
		pending_since_ms = 0U;
		prev_year = RTC_CAL_YEAR_DEFAULT;
		prev_month_day = RTC_CAL_MONTH_DAY_DEFAULT;
		prev_hhmm = RTC_CAL_HHMM_DEFAULT;
	}
}

/* --- power schedule: soft standby and STM32 STANDBY ----------------- */

static void hook_rtc_power_init(void)
{
	if (pwr_wakeup_from_standby()) {
		LOG_INF("[PWR] wake-up from hard standby (RTC alarm B)");
		pwr_clear_standby_flag();
	} else {
		LOG_INF("[PWR] cold start");
	}

	if (hook_rtc_trust_is_valid()) {
		LOG_INF("[PWR] RTC time trusted (time_manager trust record valid)");
		hook_rtc_log_internal_time();
	} else {
		LOG_WRN("[PWR] no trusted RTC time yet, waiting for a host or "
			"cloud calibration (holding %u/%u/%u)",
			RTC_CAL_YEAR, RTC_CAL_MONTH_DAY, RTC_CAL_HHMM);
	}

	LOG_INF("[PWR] schedule: on %02u:%02u off %02u:%02u (Beijing), "
		"soft=%u hard=%u",
		hook_reg_get(POWER_ON_TIME) >> 8,
		hook_reg_get(POWER_ON_TIME) & 0xFFU,
		hook_reg_get(POWER_OFF_TIME) >> 8,
		hook_reg_get(POWER_OFF_TIME) & 0xFFU,
		hook_reg_get(SOFT_STANDBY_ENABLE),
		hook_reg_get(STM32_STANDBY_ENABLE));

	hook_rtc_publish_time();
}

#if defined(CONFIG_HOOK_HARD_STANDBY_ENABLE)
/* beijing_h/beijing_m are Beijing time; the RTC alarm counters are UTC. */
static void hook_rtc_set_alarm_b(uint8_t beijing_h, uint8_t beijing_m)
{
	struct rtc_time alarm = {0};
	uint16_t mask = RTC_ALARM_TIME_MASK_HOUR |
			RTC_ALARM_TIME_MASK_MINUTE |
			RTC_ALARM_TIME_MASK_SECOND;

	alarm.tm_hour = hook_rtc_bj_hour_to_utc(beijing_h);
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
	if (!hook_rtc_trust_is_valid()) {
		LOG_ERR("[PWR-ERR] RTC not trusted, standby aborted");
		return;
	}

	LOG_INF("[PWR] entering STM32 hard standby");
	(void)offline_log_event(OFFLINE_LOG_CAT_POWER, OFFLINE_LOG_LEVEL_WRN,
				"entering STM32 hard standby, RTC alarm B will wake "
				"the controller");
	(void)offline_log_sync();
	k_msleep(200);

	pwr_clear_rtc_alarm_b_flag();
	pwr_clear_wakeup_flags();
	pwr_clear_standby_flag();
	pwr_enter_standby();
}
#endif /* CONFIG_HOOK_HARD_STANDBY_ENABLE */

/* Clear the alarm outputs and cut the alarm-light supply (soft standby). */
static void hook_rtc_sleep_enter(void)
{
	is_soft_standby = 1U;

	hook_reg_set(CMD_LED_SWITCH, 0U);
	hook_reg_set(CMD_BUZZER_7M, 0U);
	hook_reg_set(CMD_BUZZER_3M, 0U);
	hook_reg_input_set(STATUS_LED_SWITCH, 0U);
	hook_reg_input_set(STATUS_BUZZER, 0U);
	GPIO_WRITE(alarm_power_en_gpio, 0);
	GPIO_WRITE(pwr_led_gpio, 0);

	hook_reg_clear_error_bits(ERR_HEARTBEAT_TIMEOUT | ERR_LED_OFFLINE);
	hook_health_sleep_update(true);

	(void)offline_log_event(OFFLINE_LOG_CAT_POWER, OFFLINE_LOG_LEVEL_INF,
				"entered soft standby (outputs off)");

	LOG_INF("[PWR] soft standby entered (alarm power off)");
}

static void hook_rtc_sleep_leave(void)
{
	is_soft_standby = 0U;

	GPIO_WRITE(alarm_power_en_gpio, 1);
	/* PWD_LED is re-driven by app/hook_alarm (it mirrors the Modbus
	 * flash-light command register).
	 */
	hook_health_sleep_update(false);

	(void)offline_log_event(OFFLINE_LOG_CAT_POWER, OFFLINE_LOG_LEVEL_INF,
				"left soft standby (outputs on)");

	LOG_INF("[PWR] soft standby left (alarm power on)");
}

/*
 * True while the current Beijing time is inside the sleep window. The window
 * may wrap around midnight (off > on, e.g. 21:00 -> 06:00). Using the window
 * instead of an exact minute trigger means a device that reboots inside the
 * window returns to standby immediately instead of waiting for the next
 * power-off minute.
 */
static bool hook_rtc_in_sleep_window(uint16_t now_hhmm, uint16_t off_hhmm,
				     uint16_t on_hhmm)
{
	if (off_hhmm == on_hhmm) {
		return false;
	}

	if (off_hhmm < on_hhmm) {
		return (now_hhmm >= off_hhmm) && (now_hhmm < on_hhmm);
	}

	return (now_hhmm >= off_hhmm) || (now_hhmm < on_hhmm);
}

static void hook_rtc_schedule_check(void)
{
	struct tm bj;
	uint16_t now_hhmm;
	uint16_t off_hhmm;
	uint16_t on_hhmm;
	uint16_t soft_enable;
	uint16_t hard_enable;
	bool should_sleep;

	if (!hook_rtc_trust_is_valid()) {
		/* Without a trusted time the schedule must not be evaluated. */
		return;
	}

	if (hook_rtc_get_beijing_tm(&bj) != 0) {
		return;
	}

	now_hhmm = (uint16_t)(((uint8_t)bj.tm_hour << 8) | (uint8_t)bj.tm_min);
	off_hhmm = hook_reg_get(POWER_OFF_TIME);
	on_hhmm = hook_reg_get(POWER_ON_TIME);
	soft_enable = hook_reg_get(SOFT_STANDBY_ENABLE);
	hard_enable = hook_reg_get(STM32_STANDBY_ENABLE);

	should_sleep = hook_rtc_in_sleep_window(now_hhmm, off_hhmm, on_hhmm);

	if (!should_sleep) {
		if (is_soft_standby == 1U) {
			hook_rtc_sleep_leave();
		}
		return;
	}

	/* Inside the sleep window. Hard standby (STM32 STANDBY + RTC alarm B) is
	 * a standalone opt-in: it does not depend on SOFT_STANDBY_ENABLE, it only
	 * reuses the same output shutdown before the MCU powers down. Soft
	 * standby is the lighter variant when only that one is enabled.
	 */
	if ((soft_enable == 0U) && (hard_enable == 0U)) {
		if (is_soft_standby == 1U) {
			hook_rtc_sleep_leave();
			LOG_INF("[PWR] standby disabled by host, alarm power on");
		}
		return;
	}

	if (is_soft_standby == 0U) {
		hook_rtc_sleep_enter();
	}

#if defined(CONFIG_HOOK_HARD_STANDBY_ENABLE)
	/* Hard standby is opt-in: it is entered as soon as the device is inside
	 * the sleep window and is left again when RTC alarm B fires at the
	 * configured power-on time.
	 *
	 * pwr_enter_standby() only returns when STANDBY could not be entered, so
	 * the code below it is the failure path: log it and let the next schedule
	 * check (5 s later) retry, exactly like the CubeMX firmware retried once
	 * per second. A refused attempt therefore does not disable hard standby
	 * for the rest of the window - e.g. a device that boots inside the window
	 * with an untrusted RTC is re-evaluated as soon as the host or the cloud
	 * has calibrated the time.
	 */
	if (hard_enable == 1U) {
		hook_rtc_set_alarm_b((uint8_t)(on_hhmm >> 8),
				     (uint8_t)(on_hhmm & 0xFFU));
		hook_rtc_enter_standby();

		LOG_WRN_RATELIMIT("[PWR] hard standby not entered, retrying while "
				  "inside the sleep window");
	}
#else
	ARG_UNUSED(hard_enable);
#endif
}

static void hook_rtc_thread(void *a, void *b, void *c)
{
	uint32_t last_schedule_ms = 0U;

	(void)a;
	(void)b;
	(void)c;

	hook_rtc_power_init();

	for (;;) {
		hook_health_task_alive(HOOK_TASK_ALIVE_RTC);

		/* The calibration command is checked every second so that a host
		 * FC06/FC16 write is applied immediately; the current time is
		 * refreshed with it.
		 */
		hook_rtc_calibration_check();
		hook_rtc_publish_time();

		if ((hook_get_time_ms() - last_schedule_ms) >=
		    HOOK_RTC_SCHEDULE_PERIOD_MS) {
			last_schedule_ms = hook_get_time_ms();
			hook_rtc_schedule_check();
		}

		k_msleep(1000);
	}
}

K_THREAD_STACK_DEFINE(hook_rtc_stack, 4096);
static struct k_thread hook_rtc_thread_obj;

void hook_rtc_start(void)
{
	k_thread_create(&hook_rtc_thread_obj, hook_rtc_stack,
			K_THREAD_STACK_SIZEOF(hook_rtc_stack),
			hook_rtc_thread, NULL, NULL, NULL,
			OSAL_PRIO_BELOW_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_rtc_thread_obj, "RtcPwrSched");
}
