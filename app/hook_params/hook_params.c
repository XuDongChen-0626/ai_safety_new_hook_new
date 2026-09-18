/*
 * hook_params.c - product parameter table of the hook controller.
 */
#include "hook_params.h"

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "device_identity_service.h"
#include "device_param_server.h"
#include "offline_log_service.h"

LOG_MODULE_REGISTER(hook_params, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_PARAM_DEVICE_NO "device.no"

#define HOOK_PARAM_VALUE_MAX 24U

static int hook_param_changed(const char *key, const char *value, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_INF("parameter %s set to \"%s\"", key, (value != NULL) ? value : "");
	hook_params_refresh();

	return 0;
}

static const struct device_param_desc hook_param_table[] = {
	{
		/* Factory device number, e.g. "ais011". Empty = use the hardware
		 * derived short id for logs and MQTT topics.
		 */
		.key = HOOK_PARAM_DEVICE_NO,
		.type = DEVICE_PARAM_TYPE_STRING,
		.default_value = "",
		.flags = DEVICE_PARAM_F_PERSISTENT,
		.on_change = hook_param_changed,
	},
};

static char hook_param_device_no[HOOK_PARAM_VALUE_MAX];
static char hook_param_device_tag[HOOK_PARAM_VALUE_MAX];

const char *hook_params_device_no(void)
{
	return hook_param_device_no;
}

const char *hook_params_device_tag(void)
{
	return hook_param_device_tag;
}

void hook_params_refresh(void)
{
	const char *short_id;

	if (device_param_server_get(HOOK_PARAM_DEVICE_NO, hook_param_device_no,
				    sizeof(hook_param_device_no)) != 0) {
		hook_param_device_no[0] = '\0';
	}

	if (hook_param_device_no[0] != '\0') {
		(void)snprintk(hook_param_device_tag,
			       sizeof(hook_param_device_tag), "%s",
			       hook_param_device_no);
		return;
	}

	/* No factory number yet: fall back to the hardware derived short id so
	 * that logs and topics are still device specific.
	 */
	short_id = device_identity_get_short_id();
	(void)snprintk(hook_param_device_tag, sizeof(hook_param_device_tag), "%s",
		       (short_id != NULL) ? short_id : "unknown");
}

int hook_params_init(void)
{
	int ret;

	ret = device_param_server_register_table(hook_param_table,
						 ARRAY_SIZE(hook_param_table));
	if (ret != 0) {
		LOG_ERR("parameter table rejected: %d", ret);
		return ret;
	}

	hook_params_refresh();

	LOG_INF("hook parameters ready (device.no=\"%s\", tag=\"%s\")",
		hook_param_device_no, hook_param_device_tag);

	return 0;
}

/*
 * POST_KERNEL 90: after the parameter server (70) and the identity service
 * (80) have registered their tables, so the settings subtree is loaded once
 * and our values come from the QSPI param-store instead of the defaults.
 */
static int hook_params_sys_init(void)
{
	return hook_params_init();
}

SYS_INIT(hook_params_sys_init, POST_KERNEL, 90);

#if defined(CONFIG_OFFLINE_LOG_SERVICE)
/*
 * device_param_server keeps every parameter below this Settings subtree.
 * Walking it lets this application list the parameters other modules
 * registered (identity labels) without knowing their keys.
 */
#define HOOK_PARAM_SETTINGS_SUBTREE "param"
/* How often the parameter server is checked for a completed "param save". */
#define HOOK_PARAM_SAVE_POLL_MS 1000U

static uint32_t hook_param_last_save_count;
static struct k_work_delayable hook_param_save_work;

/*
 * Keys that module/device_identity_service registers through the same
 * parameter server (IDENTITY_*_KEY in that submodule).
 *
 * A parameter that still holds its default value has no entry in the Settings
 * param/ subtree at all, so the subtree walk below cannot see it. These keys
 * are therefore read explicitly, which is also why the boot dump shows the
 * identity labels before the first "param save".
 */
static const char *const hook_param_external_keys[] = {
	"id.company",
	"id.model",
	"id.product",
};

static bool hook_param_is_known_key(const char *key)
{
	for (size_t i = 0U; i < ARRAY_SIZE(hook_param_table); i++) {
		if (strcmp(hook_param_table[i].key, key) == 0) {
			return true;
		}
	}

	for (size_t i = 0U; i < ARRAY_SIZE(hook_param_external_keys); i++) {
		if (strcmp(hook_param_external_keys[i], key) == 0) {
			return true;
		}
	}

	return false;
}

static void hook_param_log_value(const char *key, const char *value,
				 const char *reason)
{
	(void)offline_log_event(OFFLINE_LOG_CAT_PARAM, OFFLINE_LOG_LEVEL_INF,
				"param %s=\"%s\" (%s)", key,
				(value != NULL) ? value : "", reason);
}

static int hook_param_log_stored_entry(const char *key, size_t len,
				       settings_read_cb read_cb, void *cb_arg,
				       void *param)
{
	char value[CONFIG_DEVICE_PARAM_SERVER_MAX_VALUE_LEN];
	const char *reason = param;
	ssize_t read_len;

	/* Local parameters are dumped from the parameter server itself, so a
	 * parameter that still holds its default value is covered as well.
	 */
	if (hook_param_is_known_key(key)) {
		return 0;
	}

	if (len >= sizeof(value)) {
		return 0;
	}

	read_len = read_cb(cb_arg, value, sizeof(value) - 1U);
	if (read_len < 0) {
		return 0;
	}

	value[read_len] = '\0';
	hook_param_log_value(key, value, reason);

	return 0;
}

void hook_params_log_all(const char *reason)
{
	char value[CONFIG_DEVICE_PARAM_SERVER_MAX_VALUE_LEN];

	if (reason == NULL) {
		reason = "";
	}

	/* Current values of the product table, including the defaults of
	 * parameters that have never been written to the param-store.
	 */
	for (size_t i = 0U; i < ARRAY_SIZE(hook_param_table); i++) {
		if (device_param_server_get(hook_param_table[i].key, value,
					    sizeof(value)) != 0) {
			(void)snprintk(value, sizeof(value), "<unavailable>");
		}

		hook_param_log_value(hook_param_table[i].key, value, reason);
	}

	/* Identity labels: present in the parameter server even when they have
	 * never been saved (default value), unlike the subtree walk below.
	 */
	for (size_t i = 0U; i < ARRAY_SIZE(hook_param_external_keys); i++) {
		if (device_param_server_get(hook_param_external_keys[i], value,
					    sizeof(value)) != 0) {
			continue;
		}

		hook_param_log_value(hook_param_external_keys[i], value,
				     reason);
	}

	/* Everything else with a stored value (identity labels registered by
	 * any other module through the same parameter server).
	 */
	(void)settings_load_subtree_direct(HOOK_PARAM_SETTINGS_SUBTREE,
					   hook_param_log_stored_entry,
					   (void *)reason);
}

/*
 * device_param_server bumps save_count on every "param save" that really
 * wrote a value, and the parameter table is the only writer - so watching
 * that counter records every save without touching the submodule and without
 * guessing which interface (shell, cloud command, code) triggered it.
 */
static void hook_param_save_work_handler(struct k_work *work)
{
	struct device_param_status status;

	ARG_UNUSED(work);

	device_param_server_get_status(&status);
	if (status.save_count != hook_param_last_save_count) {
		hook_param_last_save_count = status.save_count;
		LOG_INF("parameter save #%u detected, dumping to offline log",
			(unsigned int)status.save_count);
		hook_params_log_all("saved snapshot");
	}

	(void)k_work_reschedule(&hook_param_save_work,
				K_MSEC(HOOK_PARAM_SAVE_POLL_MS));
}

/*
 * APPLICATION 96: after the offline log service (95) has written its boot
 * events, dump the parameter list so every log file carries it.
 */
static int hook_params_log_at_boot(void)
{
	hook_params_log_all("boot snapshot");

	return 0;
}

SYS_INIT(hook_params_log_at_boot, APPLICATION, 96);

/* APPLICATION 97: start watching for completed parameter saves. */
static int hook_params_save_watch_init(void)
{
	struct device_param_status status;

	device_param_server_get_status(&status);
	hook_param_last_save_count = status.save_count;

	k_work_init_delayable(&hook_param_save_work,
			      hook_param_save_work_handler);
	(void)k_work_reschedule(&hook_param_save_work,
				K_MSEC(HOOK_PARAM_SAVE_POLL_MS));

	return 0;
}

SYS_INIT(hook_params_save_watch_init, APPLICATION, 97);
#endif /* CONFIG_OFFLINE_LOG_SERVICE */
