#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/util.h>

#define LOG_FILTER_THREAD_STACK_SIZE 768
#define LOG_FILTER_THREAD_PRIORITY 10
#define LOG_FILTER_START_DELAY_MS 1500
#define LOG_FILTER_RETRY_PERIOD_MS 1000
#define LOG_FILTER_RETRY_COUNT 5

struct runtime_log_filter {
	const char *source_name;
	uint32_t level;
};

static const struct runtime_log_filter filters[] = {
	{"modbus", LOG_LEVEL_ERR},
	{"hook_alarm", LOG_LEVEL_ERR},
	{"hook_bms", LOG_LEVEL_ERR},
	{"hook_health", LOG_LEVEL_ERR},
};

static void apply_runtime_log_filter(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(filters); i++) {
		int source_id = log_source_id_get(filters[i].source_name);

		if (source_id >= 0) {
			(void)log_filter_set(NULL, Z_LOG_LOCAL_DOMAIN_ID,
					     source_id, filters[i].level);
		}
	}
}

static void log_runtime_filter_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_msleep(LOG_FILTER_START_DELAY_MS);

	/*
	 * Zephyr activates autostart log backends from its logging thread.
	 * Backend activation reapplies the default filter, so set the log
	 * levels after startup and retry briefly in case activation is delayed.
	 */
	for (uint32_t i = 0U; i < LOG_FILTER_RETRY_COUNT; i++) {
		apply_runtime_log_filter();
		k_msleep(LOG_FILTER_RETRY_PERIOD_MS);
	}
}

K_THREAD_DEFINE(log_runtime_filter_tid, LOG_FILTER_THREAD_STACK_SIZE,
		log_runtime_filter_thread, NULL, NULL, NULL,
		LOG_FILTER_THREAD_PRIORITY, 0, 0);
