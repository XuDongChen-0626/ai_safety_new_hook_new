/*
 * hook_4g_ntp.c - trusted time from the WH-LTE-7S1 NTP client.
 *
 * The module performs NTP synchronization itself. This application-side
 * service only reads AT+CCLK after the MQTT configuration is ready, once at
 * boot and then once per hour. It never runs while the module is in HTTPD
 * mode because serial traffic in that mode belongs to the HTTP request.
 */
#include "hook_4g_ntp.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/sys/util.h>

#include "hook_4g.h"
#include "hook_4g_cfg.h"
#include "offline_log_service.h"
#include "osal_zephyr.h"
#include "time_manager_service.h"
#include "time_source_rtc.h"

LOG_MODULE_REGISTER(hook_4g_ntp, CONFIG_LOG_DEFAULT_LEVEL);

#define NTP_THREAD_STACK_SIZE 3072U
#define NTP_READY_POLL_MS 1000U
#define NTP_ONLINE_POLL_MS 5000U
#define NTP_INTERVAL_POLL_MAX_MS 60000U
#define NTP_TIME_UNCERTAINTY_MS 1000
#define NTP_CCLK_RESP_MAX 192U
#define NTP_STATUS_RESP_MAX 96U

/*
 * Greater than 2026-10-01 00:00:00 UTC+8. The module returns 2000/01/01
 * until its NTP client has obtained network time.
 */
#define NTP_MIN_TRUSTED_UTC_MS 1790784000000LL

static K_MUTEX_DEFINE(ntp_query_gate);
static K_SEM_DEFINE(ntp_wake_sem, 0, 1);
static atomic_t ntp_paused;
static bool ntp_had_success;
static bool ntp_error_reported;

static bool ntp_year_is_leap(int year)
{
	return ((year % 4) == 0 && (year % 100) != 0) ||
	       ((year % 400) == 0);
}

static bool ntp_date_is_valid(int year, int month, int day)
{
	static const uint8_t days_per_month[] = {
		31U, 28U, 31U, 30U, 31U, 30U,
		31U, 31U, 30U, 31U, 30U, 31U,
	};
	int max_day;

	if ((year < 2000) || (year > 2099) ||
	    (month < 1) || (month > 12) || (day < 1)) {
		return false;
	}

	max_day = days_per_month[month - 1];
	if ((month == 2) && ntp_year_is_leap(year)) {
		max_day = 29;
	}

	return day <= max_day;
}

/*
 * Parse "+CCLK: \"yy/MM/dd,hh:mm:ss+zz\"".
 *
 * zz is the local timezone in quarter-hour units, as used by 3GPP AT+CCLK.
 * The time manager and RTC store UTC, so the parsed local value is converted
 * back to UTC here.
 */
static int ntp_parse_cclk(const char *resp, int64_t *utc_ms,
			  int *local_year, int *local_month, int *local_day,
			  int *local_hour, int *local_min, int *local_sec)
{
	const char *p;
	struct tm tm = {0};
	char tz_sign;
	int year;
	int month;
	int day;
	int hour;
	int minute;
	int second;
	int tz_quarters;
	int matched;
	int64_t local_s;
	int64_t utc_s;

	if ((resp == NULL) || (utc_ms == NULL)) {
		return -EINVAL;
	}

	p = strstr(resp, "+CCLK:");
	if (p == NULL) {
		return -EBADMSG;
	}
	p += strlen("+CCLK:");
	while ((*p == ' ') || (*p == '\t') || (*p == '"')) {
		p++;
	}

	matched = sscanf(p, "%2d/%2d/%2d,%2d:%2d:%2d%c%2d",
			 &year, &month, &day, &hour, &minute, &second,
			 &tz_sign, &tz_quarters);
	if (matched != 8) {
		return -EBADMSG;
	}

	if (!ntp_date_is_valid(2000 + year, month, day) ||
	    (hour < 0) || (hour > 23) ||
	    (minute < 0) || (minute > 59) ||
	    (second < 0) || (second > 59) ||
	    ((tz_sign != '+') && (tz_sign != '-')) ||
	    (tz_quarters < 0) || (tz_quarters > 96)) {
		return -ERANGE;
	}

	tm.tm_year = (2000 + year) - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_sec = second;
	tm.tm_isdst = 0;

	local_s = timeutil_timegm64(&tm);
	if (local_s <= 0) {
		return -ERANGE;
	}

	utc_s = local_s -
		(int64_t)((tz_sign == '+') ? tz_quarters : -tz_quarters) *
		(15LL * 60LL);
	if (utc_s <= 0) {
		return -ERANGE;
	}

	*utc_ms = utc_s * 1000LL;
	if (local_year != NULL) {
		*local_year = 2000 + year;
		*local_month = month;
		*local_day = day;
		*local_hour = hour;
		*local_min = minute;
		*local_sec = second;
	}

	return 0;
}

static void ntp_format_utc(int64_t utc_ms, char *buf, size_t len)
{
	time_t seconds = (time_t)(utc_ms / 1000LL);
	struct tm tm;

	if ((buf == NULL) || (len == 0U) ||
	    (gmtime_r(&seconds, &tm) == NULL)) {
		if ((buf != NULL) && (len > 0U)) {
			(void)snprintf(buf, len, "invalid UTC time");
		}
		return;
	}

	(void)snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d UTC",
		       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		       tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void ntp_report_failure(int error)
{
	(void)time_manager_mark_source_error(TIME_SOURCE_NTP, error);
	if (!ntp_error_reported) {
		(void)offline_log_event(
			OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_WRN,
			"4g NTP time unavailable (%d), RTC left unchanged",
			error);
		ntp_error_reported = true;
	}
}

static int ntp_sync_once(void)
{
	char resp[NTP_CCLK_RESP_MAX];
	char utc_text[48];
	int64_t utc_ms = 0;
	int local_year = 0;
	int local_month = 0;
	int local_day = 0;
	int local_hour = 0;
	int local_min = 0;
	int local_sec = 0;
	int ret;
	int rtc_ret;

	ret = hook_4g_at_cmd("AT+CCLK", resp, sizeof(resp),
			     CONFIG_HOOK_4G_NTP_QUERY_TIMEOUT_MS);
	if (ret != 0) {
		LOG_WRN("AT+CCLK failed: %d", ret);
		ntp_report_failure(ret);
		return ret;
	}

	ret = ntp_parse_cclk(resp, &utc_ms, &local_year, &local_month,
			    &local_day, &local_hour, &local_min, &local_sec);
	if (ret != 0) {
		LOG_WRN("AT+CCLK parse failed: %d (%s)", ret, resp);
		ntp_report_failure(ret);
		return ret;
	}

	if (utc_ms <= NTP_MIN_TRUSTED_UTC_MS) {
		LOG_WRN("4G module time is not trusted yet: "
			"%04d-%02d-%02d %02d:%02d:%02d",
			local_year, local_month, local_day, local_hour,
			local_min, local_sec);
		ntp_report_failure(-ERANGE);
		return -ERANGE;
	}

	/*
	 * Refresh the running RTC first. With the project's
	 * first-trusted-source policy this also keeps RTC as the active clock,
	 * preserving host/cloud RTC calibration semantics. Then register NTP
	 * as the higher-quality reference source.
	 */
	rtc_ret = time_source_rtc_update_from_trusted_time(utc_ms);
	ret = time_manager_update_source(TIME_SOURCE_NTP, utc_ms,
					 NTP_TIME_UNCERTAINTY_MS, true, 0);
	if (ret != 0) {
		LOG_ERR("time manager rejected 4G NTP time: %d", ret);
		ntp_report_failure(ret);
		return ret;
	}

	ntp_format_utc(utc_ms, utc_text, sizeof(utc_text));
	LOG_INF("4G NTP time trusted: %s (%04d-%02d-%02d %02d:%02d:%02d local)",
		utc_text, local_year, local_month, local_day, local_hour,
		local_min, local_sec);

	if (rtc_ret != 0) {
		LOG_ERR("4G NTP time is valid but the RTC update failed: %d",
			rtc_ret);
		(void)offline_log_event(
			OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_ERR,
			"4g NTP time valid but RTC update failed (%d)", rtc_ret);
	} else if (!ntp_had_success || ntp_error_reported) {
		(void)offline_log_event(
			OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_INF,
			"4g NTP time synchronized: %s", utc_text);
	}

	ntp_had_success = true;
	ntp_error_reported = false;

	return 0;
}

static bool ntp_query_allowed(void)
{
	return (hook_4g_cfg_get_state() == HOOK_4G_CFG_DONE) &&
	       !hook_4g_is_http_mode() &&
	       (atomic_get(&ntp_paused) == 0);
}

static int ntp_wait_for_mqtt_connection(void)
{
	char resp[NTP_STATUS_RESP_MAX];

	while (ntp_query_allowed()) {
		if (hook_4g_at_cmd("AT+MQTTSTA", resp, sizeof(resp),
				   CONFIG_HOOK_4G_NTP_QUERY_TIMEOUT_MS) == 0) {
			const char *value = strchr(resp, ':');

			if (value != NULL) {
				value++;
				/*
				 * WH-LTE-7S1 answers "+MQTTSTA:\"CONNECTION\"".
				 * Strip the optional quotes as well as whitespace,
				 * otherwise the NTP thread never reaches AT+CCLK.
				 */
				while ((*value == ' ') || (*value == '\t') ||
				       (*value == '"')) {
					value++;
				}
				if (strncmp(value, "CONNECTION",
					    strlen("CONNECTION")) == 0) {
					return 0;
				}
			}
		}

		/*
		 * A disconnected MQTT client does not consume the once-at-boot
		 * or hourly read: wait for the module/network rather than
		 * recording a bogus 2000/01/01 NTP failure.
		 */
		k_msleep(NTP_ONLINE_POLL_MS);
	}

	return -ECANCELED;
}

static void ntp_wait_until_allowed(void)
{
	while (!ntp_query_allowed()) {
		k_msleep(NTP_READY_POLL_MS);
	}
}

static void ntp_wait_until_due(int64_t next_ms)
{
	for (;;) {
		int64_t remaining = next_ms - k_uptime_get();
		uint32_t wait_ms;

		if (remaining <= 0) {
			return;
		}

		wait_ms = (uint32_t)MIN(remaining, (int64_t)NTP_INTERVAL_POLL_MAX_MS);
		if (k_sem_take(&ntp_wake_sem, K_MSEC(wait_ms)) == 0) {
			/*
			 * A pause/resume woke us early. If the query is currently
			 * blocked, go back to readiness polling; otherwise keep
			 * waiting for the original hourly deadline.
			 */
			if (!ntp_query_allowed()) {
				return;
			}
			continue;
		}

		if (!ntp_query_allowed()) {
			return;
		}
	}
}

static void hook_4g_ntp_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	k_thread_name_set(k_current_get(), "Hook4gNtp");

	while (true) {
		int64_t next_ms;
		bool queried = false;

		ntp_wait_until_allowed();

		/*
		 * Pause HTTP OTA can arrive after ntp_wait_until_allowed().
		 * Holding the gate makes the pause operation wait for an
		 * in-flight AT+CCLK exchange, and prevents a new one from
		 * interleaving with the HTTPD transition.
		 */
		k_mutex_lock(&ntp_query_gate, K_FOREVER);
		if ((ntp_query_allowed()) &&
		    (ntp_wait_for_mqtt_connection() == 0)) {
			(void)ntp_sync_once();
			queried = true;
		}
		k_mutex_unlock(&ntp_query_gate);

		if (!queried) {
			continue;
		}

		next_ms = k_uptime_get() +
			  (int64_t)CONFIG_HOOK_4G_NTP_READ_INTERVAL_S * 1000LL;
		ntp_wait_until_due(next_ms);
	}
}

K_THREAD_STACK_DEFINE(hook_4g_ntp_stack, NTP_THREAD_STACK_SIZE);
static struct k_thread hook_4g_ntp_thread_obj;

void hook_4g_ntp_start(void)
{
	k_thread_create(&hook_4g_ntp_thread_obj, hook_4g_ntp_stack,
			K_THREAD_STACK_SIZEOF(hook_4g_ntp_stack),
			hook_4g_ntp_thread, NULL, NULL, NULL,
			OSAL_PRIO_BELOW_NORMAL1, 0, K_NO_WAIT);
}

void hook_4g_ntp_set_paused(bool paused)
{
	atomic_set(&ntp_paused, paused ? 1 : 0);

	if (paused) {
		/* Wait for an AT+CCLK exchange that started just before this. */
		k_mutex_lock(&ntp_query_gate, K_FOREVER);
		k_mutex_unlock(&ntp_query_gate);
	} else {
		/* Re-check the hourly deadline instead of sleeping through it. */
		k_sem_give(&ntp_wake_sem);
	}
}
