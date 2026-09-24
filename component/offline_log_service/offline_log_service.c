/*
 * offline_log_service.c - curated event API and boot/network/time watchers.
 *
 * Product code calls offline_log_event() only for important state changes
 * (device online/offline, power transitions, network link changes, OTA
 * lifecycle, reset reasons, ...). The Zephyr LOG stream is intentionally not
 * mirrored to the SD card.
 */

#include "offline_log_internal.h"
#include "offline_log_service.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>

#include <zephyr/app_version.h>

#if defined(CONFIG_DEVICE_IDENTITY_SERVICE)
#include "device_identity_service.h"
#endif
#if defined(CONFIG_NETWORK_MANAGER_SERVICE_EVENT_CALLBACKS)
#endif
#if defined(CONFIG_TIME_MANAGER_SERVICE)
#include "time_manager_service.h"
#endif
#if defined(CONFIG_FAULT_DUMP_SERVICE)
#include "fault_dump_service.h"
#endif
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(offline_log, CONFIG_LOG_DEFAULT_LEVEL);

#define OL_MSG_MAX CONFIG_OFFLINE_LOG_MSG_MAX

#if defined(CONFIG_FAULT_DUMP_SERVICE)
/*
 * Keep the fault record available for `faultdump show` while making sure the
 * same retained fatal is written into the offline log only once, even across
 * warm resets. Kept in noinit RAM (component layer) so the fault_dump_service
 * submodule stays untouched.
 */
#define OL_FATAL_REPORT_MAGIC 0x4f4c4642U /* OLFB */

struct ol_fatal_report_state {
	uint32_t magic;
	uint32_t last_reported_boot;
};

static struct ol_fatal_report_state fatal_report_state __noinit;

static bool fatal_report_needed(uint32_t fault_boot_count)
{
	if (fatal_report_state.magic != OL_FATAL_REPORT_MAGIC) {
		fatal_report_state.magic = OL_FATAL_REPORT_MAGIC;
		fatal_report_state.last_reported_boot = UINT32_MAX;
	}

	return fatal_report_state.last_reported_boot != fault_boot_count;
}

static void fatal_report_mark_done(uint32_t fault_boot_count)
{
	if (fatal_report_state.magic != OL_FATAL_REPORT_MAGIC) {
		fatal_report_state.magic = OL_FATAL_REPORT_MAGIC;
	}
	fatal_report_state.last_reported_boot = fault_boot_count;
}
#endif

static const char *const category_names[OFFLINE_LOG_CAT_COUNT] = {
	[OFFLINE_LOG_CAT_SYSTEM]  = "system",
	[OFFLINE_LOG_CAT_POWER]   = "power",
	[OFFLINE_LOG_CAT_NETWORK] = "4g",
	[OFFLINE_LOG_CAT_BMS]     = "bms",
	[OFFLINE_LOG_CAT_ALARM]   = "alarm",
	[OFFLINE_LOG_CAT_OTA]     = "ota",
	[OFFLINE_LOG_CAT_FAULT]   = "fault",
	[OFFLINE_LOG_CAT_PARAM]   = "param",
};

static const char *const level_names[] = {
	[0] = "???",
	[OFFLINE_LOG_LEVEL_ERR] = "err",
	[OFFLINE_LOG_LEVEL_WRN] = "wrn",
	[OFFLINE_LOG_LEVEL_INF] = "inf",
	[OFFLINE_LOG_LEVEL_DBG] = "dbg",
};

const char *offline_log_category_name(enum offline_log_category cat)
{
	if (cat < 0 || cat >= OFFLINE_LOG_CAT_COUNT) {
		return "?";
	}
	return category_names[cat];
}

const char *offline_log_level_name(enum offline_log_level level)
{
	if (level < OFFLINE_LOG_LEVEL_ERR || level > OFFLINE_LOG_LEVEL_DBG) {
		return "???";
	}
	return level_names[level];
}

static void mirror_to_log(enum offline_log_category cat,
			  enum offline_log_level level,
			  const char *text)
{
	const char *tag = offline_log_category_name(cat);

	switch (level) {
	case OFFLINE_LOG_LEVEL_ERR:
		LOG_ERR("[%s] %s", tag, text);
		break;
	case OFFLINE_LOG_LEVEL_WRN:
		LOG_WRN("[%s] %s", tag, text);
		break;
	case OFFLINE_LOG_LEVEL_INF:
		LOG_INF("[%s] %s", tag, text);
		break;
	default:
		LOG_DBG("[%s] %s", tag, text);
		break;
	}
}

int offline_log_event(enum offline_log_category cat,
		      enum offline_log_level level,
		      const char *fmt, ...)
{
	char text[OL_MSG_MAX];
	va_list ap;
	int n;

	if (fmt == NULL) {
		return -EINVAL;
	}

	va_start(ap, fmt);
	n = vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	if (n < 0) {
		return -EINVAL;
	}
	if ((size_t)n >= sizeof(text)) {
		n = (int)sizeof(text) - 1;
	}

	mirror_to_log(cat, level, text);
	(void)offline_log_sd_enqueue(cat, level, text);
	return 0;
}

void offline_log_transition(enum offline_log_category cat,
			    enum offline_log_level level,
			    bool *current, bool next,
			    const char *on_msg, const char *off_msg)
{
	if (current == NULL) {
		return;
	}
	if (*current == next) {
		return;
	}
	*current = next;

	if (next) {
		if (on_msg != NULL) {
			(void)offline_log_event(cat, level, "%s", on_msg);
		}
	} else {
		if (off_msg != NULL) {
			(void)offline_log_event(cat, level, "%s", off_msg);
		}
	}
}

int offline_log_sync(void)
{
	return offline_log_sd_sync();
}

void offline_log_get_status(struct offline_log_status *out)
{
	offline_log_sd_get_status(out);
}

/* ------------------------------------------------------------------ */
/* boot-time watcher: record identity, reset reason, OTA boot state    */
/* ------------------------------------------------------------------ */

static void log_boot_events(void)
{
	char text[OL_MSG_MAX];

	(void)offline_log_event(OFFLINE_LOG_CAT_SYSTEM, OFFLINE_LOG_LEVEL_INF,
				"system boot fw=%d.%d.%d board=%s boot_ms=%u",
				APP_VERSION_MAJOR, APP_VERSION_MINOR,
				APP_PATCHLEVEL, CONFIG_BOARD,
				k_uptime_get_32());

#if defined(CONFIG_DEVICE_IDENTITY_SERVICE)
	{
		const struct device_identity *id = device_identity_get_info();

		if (id != NULL) {
			(void)snprintf(text, sizeof(text),
				       "device product=%s model=%s host=%s "
				       "uid=%s short=%s",
				       id->product, id->model, id->hostname,
				       id->hardware_uid, id->short_id);
			(void)offline_log_event(OFFLINE_LOG_CAT_SYSTEM,
						OFFLINE_LOG_LEVEL_INF, "%s",
						text);
		}
	}
#endif

#if defined(CONFIG_FAULT_DUMP_SERVICE)
	{
		struct fault_dump_service_summary summary;

		if (fault_dump_service_get_summary(&summary) == 0) {
			enum offline_log_level lvl = OFFLINE_LOG_LEVEL_INF;

			if (strcmp(summary.reset_class, "watchdog") == 0 ||
			    strcmp(summary.reset_class, "cpu") == 0) {
				lvl = OFFLINE_LOG_LEVEL_WRN;
			}
			(void)offline_log_event(
				OFFLINE_LOG_CAT_FAULT, lvl,
				"boot reset class=%s reason=%s conclusion=%s",
				summary.reset_class, summary.reset_reason,
				summary.conclusion);
		}

		if (fault_dump_service_has_dump()) {
			struct fault_dump_service_record record;
			int fatal_rc;

			if (fault_dump_service_get_record(&record) == 0 &&
			    record.valid &&
			    fatal_report_needed(record.boot_count)) {
				fatal_rc = offline_log_event(
					OFFLINE_LOG_CAT_FAULT,
					OFFLINE_LOG_LEVEL_ERR,
					"previous fatal reason=%u msg=%s "
					"thread=%s uptime_ms=%u pc=0x%08x "
					"lr=0x%08x",
					record.reason,
					fault_dump_service_reason_to_msg(
						record.reason),
					record.thread_name, record.uptime_ms,
					record.pc, record.lr);
				if (fatal_rc == 0) {
					fatal_report_mark_done(
						record.boot_count);
				}
			}
		}
	}
#endif

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	{
		int st = mcuboot_swap_type();

		switch (st) {
		case BOOT_SWAP_TYPE_TEST:
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"OTA test boot: new image is pending "
				"confirmation");
			break;
		case BOOT_SWAP_TYPE_REVERT:
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
				"OTA test image running without confirmation: "
				"MCUboot will revert to the previous image on the "
				"next reboot unless it is confirmed");
			break;
		case BOOT_SWAP_TYPE_FAIL:
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
				"OTA swap failed (MCUboot FAIL state)");
			break;
		default:
			break;
		}
	}
#endif
}

/* ------------------------------------------------------------------ */
/* runtime watchers                                                    */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_NETWORK_MANAGER_SERVICE_EVENT_CALLBACKS)
static void network_event_cb(enum network_manager_service_event event,
			     const struct network_manager_service_status *st,
			     void *user_data)
{
	enum offline_log_level lvl = OFFLINE_LOG_LEVEL_INF;

	ARG_UNUSED(user_data);

	switch (event) {
	case NETWORK_MANAGER_SERVICE_EVENT_CARRIER_OFF:
	case NETWORK_MANAGER_SERVICE_EVENT_IPV4_LOST:
	case NETWORK_MANAGER_SERVICE_EVENT_ERROR:
		lvl = OFFLINE_LOG_LEVEL_WRN;
		break;
	default:
		break;
	}

	(void)offline_log_event(
		OFFLINE_LOG_CAT_NETWORK, lvl,
		"network %s state=%s ip=%s err=%d",
		network_manager_service_event_name(event),
		network_manager_service_state_name(st->state),
		st->ipv4_addr[0] != '\0' ? st->ipv4_addr : "-",
		st->last_error);
}
#endif

#if defined(CONFIG_TIME_MANAGER_SERVICE)
static void trusted_time_cb(enum time_source_id source, int64_t utc_ms,
			    void *user_data)
{
	char iso[40];

	ARG_UNUSED(user_data);
	(void)time_manager_format_local_iso8601_from_utc_ms(utc_ms, iso,
							   sizeof(iso));
	(void)offline_log_event(
		OFFLINE_LOG_CAT_SYSTEM, OFFLINE_LOG_LEVEL_INF,
		"trusted time acquired source=%s local=%s",
		time_manager_source_name(source), iso);
}
#endif

/* ------------------------------------------------------------------ */
/* service init                                                        */
/* ------------------------------------------------------------------ */

static int offline_log_service_init(void)
{
	(void)offline_log_sd_start();
	log_boot_events();

#if defined(CONFIG_NETWORK_MANAGER_SERVICE_EVENT_CALLBACKS)
	(void)network_manager_service_register_callback(network_event_cb,
							NULL);
#endif
#if defined(CONFIG_TIME_MANAGER_SERVICE)
	(void)time_manager_register_trusted_time_callback(trusted_time_cb,
							  NULL);
#endif

	return 0;
}

/* APPLICATION 95: after network (30), time manager (40), OTA manager (80)
 * and fault dump service (90) have initialized, before most business threads
 * report their first state changes.
 */
SYS_INIT(offline_log_service_init, APPLICATION, 95);
