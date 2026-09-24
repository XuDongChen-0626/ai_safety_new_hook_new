/*
 * hook_4g_cfg.c - automatic 4G module provisioning (MQTT mode / topics).
 *
 * Boot flow (once per MCU start, never while the device is running):
 *
 *   1. wait until the module answers (probe instead of a fixed 10 s wait)
 *   2. read AT+WKMOD
 *        not "MQTT,NOR" -> factory flow: AT+CLEAR, full rewrite, AT+S
 *        "MQTT,NOR"     -> compare the module configuration with the values
 *                          derived from device.no / mqtt.* and write only the
 *                          entries that differ
 *   3. AT+S (save + module restart), wait for the module, verify AT+MQTTSTA
 *
 * The parameters are also fingerprinted (modem.cfg_hash): a change of
 * device.no or of any mqtt.* parameter invalidates the fingerprint and forces
 * one full rewrite even for values the module cannot read back (password).
 *
 * Everything uses the synchronous hook_4g_at_cmd() channel, which does not
 * echo the exchange on the shell and does not mirror it to MQTT topic 1 - the
 * flow sends the MQTT password.
 */
#include "hook_4g_cfg.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "hook_4g.h"
#include "hook_heartbeat.h"
#include "hook_params.h"
#include "device_param_server.h"
#include "offline_log_service.h"
#include "osal_zephyr.h"

LOG_MODULE_REGISTER(hook_4g_cfg, CONFIG_LOG_DEFAULT_LEVEL);

#define CFG_DEV_MAX      32U
#define CFG_TOPIC_MAX    80U
#define CFG_CMD_MAX      160U
#define CFG_EXPECT_MAX   128U
#define CFG_RESP_MAX     768U
/* 10 single entries + 3 publish + 4 subscribe topics, with headroom. */
#define CFG_ENTRY_MAX    18U

#define CFG_BOOT_DELAY_MS         3000U
/*
 * A cold module needs tens of seconds before it answers: power up, network
 * attach, then the AT interpreter. Measured on the bench: the first answer
 * came ~40 s after reset, so a 10 s window only produced false failures.
 */
/*
 * The module is not to be talked to while it is booting: it prints its banner
 * ("WH-LTE-7S1") when it is up, and probing before that only produced
 * unanswered commands. Wait for the banner first, with a bounded fallback for
 * builds that hide it.
 */
#define CFG_BANNER_WAIT_MS        15000U
#define CFG_READY_TIMEOUT_MS      60000U
/* Answers can be slow while the module is attaching / connecting MQTT. */
#define CFG_READY_PROBE_MS        5000U
#define CFG_READY_PROBE_PERIOD_MS 500U
#define CFG_CMD_TIMEOUT_MS        5000U
/*
 * The module behaves like this after AT+S: it restarts, comes back, and then
 * restarts a second time (~11 s later) while applying the new MQTT settings -
 * so an answer that is "CONNECTION" once is not yet proof of a stable link.
 */
#define CFG_LINK_WAIT_MS          30000U
#define CFG_LINK_POLL_MS          1000U
/* Consecutive CONNECTION answers required before the link is declared up. */
#define CFG_LINK_STABLE_COUNT     2U
#define CFG_ATTEMPTS              3U
#define CFG_RETRY_DELAY_MS        2000U
#define CFG_HASH_KEY              "modem.cfg_hash"
/*
 * Settling time between "module has an IP address" and "the link really
 * forwards data": measured ~5 s on the bench (the network LED keeps blinking).
 */
#define HOOK_4G_CFG_HTTP_NET_SETTLE_MS 5000U
/*
 * "Wait until the network is up" probe (AT+CIP). It polls on purpose: the
 * module answers AT+CIP normally long before its network is usable, and an
 * answer that never got an OK used to cost the full 5 s command timeout, so
 * twenty tries turned into two minutes of dead time.
 */
#define HOOK_4G_CFG_HTTP_IP_PROBE_MS 1500U
#define HOOK_4G_CFG_HTTP_IP_TRIES    12U

/* Fixed by the cloud protocol (AISatefy-Hook-MQTT通信协议-v2.0.0). */
#define CFG_TOPIC_PREFIX      "ai_safety"
#define CFG_CID_SUFFIX        "-hook"
#define CFG_WKMOD             "MQTT,NOR"
#define CFG_MQTTMOD           "1"
#define CFG_MQTTCFG           "60,1"
#define CFG_HEARTEN           "OFF"
#define CFG_REGEN             "OFF"
#define CFG_RSTIM             "0"
#define CFG_PUB_QOS           "2"
#define CFG_PUB_RETAIN        "0"
#define CFG_SUB_QOS           "2"
#define CFG_SUB_QOS_HEARTBEAT "0"
#define CFG_LINK_STATE_OK     "CONNECTION"

/* How an entry is compared against the module's read back value. */
enum cfg_match {
	CFG_MATCH_EXACT = 0,  /* value equals the expectation            */
	CFG_MATCH_CONTAINS,   /* value contains the expectation          */
	CFG_MATCH_LINE,       /* answer holds a line equal to the value  */
};

struct cfg_entry {
	char cmd[CFG_CMD_MAX];
	char expect[CFG_EXPECT_MAX];
	const char *read_cmd;   /* NULL: cannot be read back (password) */
	enum cfg_match match;
};

static struct cfg_entry cfg_entries[CFG_ENTRY_MAX];
static size_t cfg_entry_count;
static char cfg_dev[CFG_DEV_MAX];
static char cfg_cid[CFG_DEV_MAX + sizeof(CFG_CID_SUFFIX)];
static volatile enum hook_4g_cfg_state cfg_state;
static K_MUTEX_DEFINE(cfg_mutex);

static int cfg_at_timeout(const char *cmd, char *resp, size_t resp_len,
			  uint32_t timeout_ms)
{
	return hook_4g_at_cmd(cmd, resp, resp_len, timeout_ms);
}

static int cfg_at(const char *cmd, char *resp, size_t resp_len)
{
	return cfg_at_timeout(cmd, resp, resp_len, CFG_CMD_TIMEOUT_MS);
}

/* Value after the first ':' of an answer, quotes and line ends stripped. */
static bool cfg_resp_value(const char *resp, char *out, size_t out_len)
{
	const char *colon = strchr(resp, ':');
	const char *p;
	size_t used = 0U;

	if ((colon == NULL) || (out_len == 0U)) {
		return false;
	}

	p = &colon[1];
	while ((*p == ' ') || (*p == '\t') || (*p == '"')) {
		p++;
	}

	while ((*p != '\0') && (*p != '\r') && (*p != '\n') && (*p != '"')) {
		if ((used + 1U) < out_len) {
			out[used++] = *p;
		}
		p++;
	}

	if (used == 0U) {
		/* "+MQTTPUBTP:" - a list follows rather than a single value. */
		return false;
	}

	out[used] = '\0';

	return true;
}

/* True when the answer contains "line" as a whole (trimmed) line. */
static bool cfg_resp_has_line(const char *resp, const char *line)
{
	const char *p = resp;
	size_t want_len = strlen(line);

	while (*p != '\0') {
		const char *end = strpbrk(p, "\r\n");
		size_t len = (end != NULL) ? (size_t)(end - p) : strlen(p);

		while ((len > 0U) && ((p[0] == ' ') || (p[0] == '\t') ||
				      (p[0] == '"'))) {
			p++;
			len--;
		}
		while ((len > 0U) && ((p[len - 1U] == ' ') ||
				      (p[len - 1U] == '\t') ||
				      (p[len - 1U] == '"'))) {
			len--;
		}

		if ((len == want_len) && (strncmp(p, line, len) == 0)) {
			return true;
		}

		if (end == NULL) {
			break;
		}

		p = end;
		while ((*p == '\r') || (*p == '\n')) {
			p++;
		}
	}

	return false;
}

/*
 * Probe the module until it answers, instead of a blind wait.
 *
 * AT+WKMOD is used as the probe rather than a bare "AT": it is the first value
 * the flow needs anyway, and it is known to be answered by this module build.
 *
 * `restarting` says whether the module was just told to restart (AT+CLEAR,
 * AT+S). Only then is it worth waiting for the boot banner before the first
 * probe: the banner is the earliest proof that the AT interpreter is back, and
 * probing a module that is still booting only burns a probe timeout.
 *
 * When the module is already running (switching work mode back and forth, for
 * example) no banner is going to come, and waiting for one costs the whole
 * CFG_BANNER_WAIT_MS for nothing.
 */
static int cfg_wait_modem_wait(uint32_t timeout_ms, bool restarting)
{
	char resp[256];
	char value[32];
	uint32_t start = k_uptime_get_32();

	if (restarting) {
		while (!hook_4g_modem_boot_seen() &&
		       ((uint32_t)(k_uptime_get_32() - start) <
			CFG_BANNER_WAIT_MS)) {
			k_msleep(200);
		}
	}

	while ((uint32_t)(k_uptime_get_32() - start) < timeout_ms) {
		if ((cfg_at_timeout("AT+WKMOD", resp, sizeof(resp),
				    CFG_READY_PROBE_MS) == 0) &&
		    cfg_resp_value(resp, value, sizeof(value))) {
			return 0;
		}

		k_msleep(CFG_READY_PROBE_PERIOD_MS);
	}

	return -ETIMEDOUT;
}

static int cfg_wait_modem(uint32_t timeout_ms)
{
	return cfg_wait_modem_wait(timeout_ms, true);
}

static void cfg_add(const char *cmd, const char *expect, const char *read_cmd,
		    enum cfg_match match)
{
	struct cfg_entry *entry;

	if (cfg_entry_count >= ARRAY_SIZE(cfg_entries)) {
		return;
	}

	entry = &cfg_entries[cfg_entry_count++];
	(void)snprintk(entry->cmd, sizeof(entry->cmd), "%s", cmd);
	(void)snprintk(entry->expect, sizeof(entry->expect), "%s", expect);
	entry->read_cmd = read_cmd;
	entry->match = match;
}

static void cfg_build(void)
{
	static const char *const pub_tail[] = {
		"system/hook/server",
		"ota/hook/server",
		"inform/hook/device_manager",
	};
	static const char *const sub_tail[] = {
		"system/server/hook",
		"ota/server/hook",
		"command/device_manager/hook",
		"heartbeat/device_manager/hook",
	};
	char cmd[CFG_CMD_MAX];
	char expect[CFG_EXPECT_MAX];
	const char *device_no = hook_params_device_no();

	cfg_entry_count = 0U;

	/* device.no defaults to the hardware short id, so dev is never empty. */
	(void)snprintk(cfg_dev, sizeof(cfg_dev), "ais%s",
		       (device_no[0] != '\0') ? device_no : "unknown");
	(void)snprintk(cfg_cid, sizeof(cfg_cid), "%s%s", cfg_dev,
		       CFG_CID_SUFFIX);

	(void)snprintk(cmd, sizeof(cmd), "AT+WKMOD=%s", CFG_WKMOD);
	/* Some module builds answer plain "MQTT", so the mode is matched by
	 * containment - a strict compare would trigger the factory CLEAR on
	 * every boot.
	 */
	cfg_add(cmd, "MQTT", "AT+WKMOD", CFG_MATCH_CONTAINS);

	(void)snprintk(cmd, sizeof(cmd), "AT+MQTTMOD=%s", CFG_MQTTMOD);
	cfg_add(cmd, CFG_MQTTMOD, "AT+MQTTMOD", CFG_MATCH_EXACT);

	(void)snprintk(cmd, sizeof(cmd), "AT+MQTTCFG=%s", CFG_MQTTCFG);
	cfg_add(cmd, CFG_MQTTCFG, "AT+MQTTCFG", CFG_MATCH_EXACT);

	(void)snprintk(expect, sizeof(expect), "%s,%s",
		       hook_params_mqtt_server(), hook_params_mqtt_port());
	(void)snprintk(cmd, sizeof(cmd), "AT+MQTTSVR=%s", expect);
	cfg_add(cmd, expect, "AT+MQTTSVR", CFG_MATCH_EXACT);

	(void)snprintk(cmd, sizeof(cmd), "AT+MQTTUSER=%s",
		       hook_params_mqtt_user());
	cfg_add(cmd, hook_params_mqtt_user(), "AT+MQTTUSER", CFG_MATCH_EXACT);

	/* The password cannot be read back reliably, so it is only written
	 * (never compared); modem.cfg_hash covers changes to it.
	 */
	(void)snprintk(cmd, sizeof(cmd), "AT+MQTTPSW=%s",
		       hook_params_mqtt_password());
	cfg_add(cmd, hook_params_mqtt_password(), NULL, CFG_MATCH_EXACT);

	(void)snprintk(cmd, sizeof(cmd), "AT+MQTTCID=%s", cfg_cid);
	cfg_add(cmd, cfg_cid, "AT+MQTTCID", CFG_MATCH_EXACT);

	(void)snprintk(cmd, sizeof(cmd), "AT+HEARTEN=%s", CFG_HEARTEN);
	cfg_add(cmd, CFG_HEARTEN, "AT+HEARTEN", CFG_MATCH_EXACT);

	(void)snprintk(cmd, sizeof(cmd), "AT+REGEN=%s", CFG_REGEN);
	cfg_add(cmd, CFG_REGEN, "AT+REGEN", CFG_MATCH_EXACT);

	(void)snprintk(cmd, sizeof(cmd), "AT+RSTIM=%s", CFG_RSTIM);
	cfg_add(cmd, CFG_RSTIM, "AT+RSTIM", CFG_MATCH_EXACT);

	for (size_t i = 0U; i < ARRAY_SIZE(pub_tail); i++) {
		char topic[CFG_TOPIC_MAX];

		(void)snprintk(topic, sizeof(topic), CFG_TOPIC_PREFIX "/%s/%s",
			       cfg_dev, pub_tail[i]);
		(void)snprintk(expect, sizeof(expect), "%u,1,%s,%s,%s",
			       (unsigned int)(i + 1U), topic, CFG_PUB_QOS,
			       CFG_PUB_RETAIN);
		(void)snprintk(cmd, sizeof(cmd), "AT+MQTTPUBTP=%s", expect);
		cfg_add(cmd, expect, "AT+MQTTPUBTP", CFG_MATCH_LINE);
	}

	for (size_t i = 0U; i < ARRAY_SIZE(sub_tail); i++) {
		char topic[CFG_TOPIC_MAX];
		const char *qos = (i == (ARRAY_SIZE(sub_tail) - 1U)) ?
					  CFG_SUB_QOS_HEARTBEAT : CFG_SUB_QOS;

		(void)snprintk(topic, sizeof(topic), CFG_TOPIC_PREFIX "/%s/%s",
			       cfg_dev, sub_tail[i]);
		(void)snprintk(expect, sizeof(expect), "%u,1,%s,%s",
			       (unsigned int)(i + 1U), topic, qos);
		(void)snprintk(cmd, sizeof(cmd), "AT+MQTTSUBTP=%s", expect);
		cfg_add(cmd, expect, "AT+MQTTSUBTP", CFG_MATCH_LINE);
	}
}

/* 32 bit FNV-1a over the parameter set that shapes the module config. */
static uint32_t cfg_hash(void)
{
	const char *const parts[] = {
		cfg_dev,
		hook_params_mqtt_server(),
		hook_params_mqtt_port(),
		hook_params_mqtt_user(),
		hook_params_mqtt_password(),
		"v1",
	};
	uint32_t hash = 2166136261U;

	for (size_t i = 0U; i < ARRAY_SIZE(parts); i++) {
		for (const char *p = parts[i]; *p != '\0'; p++) {
			hash ^= (uint8_t)*p;
			hash *= 16777619U;
		}
		/* Separator so ("ab","c") and ("a","bc") differ. */
		hash ^= 0xFFU;
		hash *= 16777619U;
	}

	return hash;
}

static void cfg_hash_text(char *buf, size_t buf_len)
{
	(void)snprintk(buf, buf_len, "%08x", cfg_hash());
}

static int cfg_write_entry(const struct cfg_entry *entry)
{
	char resp[256];

	for (uint32_t attempt = 0U; attempt < 2U; attempt++) {
		int ret = cfg_at(entry->cmd, resp, sizeof(resp));

		if (ret == 0) {
			return 0;
		}

		LOG_WRN("provision: '%s' failed (%d), retry %u", entry->cmd, ret,
			(unsigned int)(attempt + 1U));

		/* An unanswered command usually means the module is restarting
		 * (some AT+MQTT* writes make it reboot): wait for it to come
		 * back instead of writing into a dead link.
		 */
		if (cfg_wait_modem(CFG_READY_TIMEOUT_MS) != 0) {
			LOG_WRN("provision: module did not come back for '%s'",
				entry->cmd);
			break;
		}
	}

	return -EIO;
}

/*
 * Compare the module configuration with the expected entries.
 *
 * Entries sharing a read command (the PUBTP/SUBTP lists) are read once. When
 * a read fails the whole group is marked as differing, so the flow rewrites it
 * instead of leaving a stale configuration behind.
 */
static void cfg_collect_diff(bool *diff, size_t *diff_count)
{
	char resp[CFG_RESP_MAX];
	const char *last_read = NULL;
	int last_ret = -1;

	*diff_count = 0U;

	for (size_t i = 0U; i < cfg_entry_count; i++) {
		const struct cfg_entry *entry = &cfg_entries[i];
		char value[CFG_EXPECT_MAX];

		diff[i] = false;

		if (entry->read_cmd == NULL) {
			continue;
		}

		if ((last_read == NULL) ||
		    (strcmp(last_read, entry->read_cmd) != 0)) {
			last_read = entry->read_cmd;
			last_ret = cfg_at(entry->read_cmd, resp, sizeof(resp));
		}

		if (last_ret != 0) {
			diff[i] = true;
		} else if (entry->match == CFG_MATCH_LINE) {
			diff[i] = !cfg_resp_has_line(resp, entry->expect);
		} else if (!cfg_resp_value(resp, value, sizeof(value))) {
			diff[i] = true;
		} else if (entry->match == CFG_MATCH_CONTAINS) {
			diff[i] = (strstr(value, entry->expect) == NULL);
		} else {
			diff[i] = (strcmp(value, entry->expect) != 0);
		}

		if (diff[i]) {
			(*diff_count)++;
		}
	}
}

/* Wait until the module reports an established MQTT connection. */
static int cfg_verify_link(void)
{
	char resp[256];
	char value[32];
	uint32_t start = k_uptime_get_32();
	uint32_t stable = 0U;

	while ((uint32_t)(k_uptime_get_32() - start) < CFG_LINK_WAIT_MS) {
		if ((cfg_at("AT+MQTTSTA", resp, sizeof(resp)) == 0) &&
		    cfg_resp_value(resp, value, sizeof(value)) &&
		    (strcmp(value, CFG_LINK_STATE_OK) == 0)) {
			/* The module restarts a second time shortly after AT+S, so
			 * only consecutive answers prove the link is stable.
			 */
			if (++stable >= CFG_LINK_STABLE_COUNT) {
				return 0;
			}
		} else {
			stable = 0U;
		}

		k_msleep(CFG_LINK_POLL_MS);
	}

	return -ETIMEDOUT;
}

static int cfg_provision_once(bool factory)
{
	char resp[CFG_RESP_MAX];
	char mode[32];
	char stored_hash[16];
	char expect_hash[16];
	bool diff[CFG_ENTRY_MAX];
	size_t diff_count = 0U;
	bool need_full = factory;
	int ret;

	cfg_state = HOOK_4G_CFG_READ;

	if ((cfg_at("AT+WKMOD", resp, sizeof(resp)) != 0) ||
	    !cfg_resp_value(resp, mode, sizeof(mode))) {
		LOG_WRN("provision: AT+WKMOD did not answer");
		return -EIO;
	}

	cfg_hash_text(expect_hash, sizeof(expect_hash));
	if (hook_params_get(CFG_HASH_KEY, stored_hash, sizeof(stored_hash)) != 0) {
		stored_hash[0] = '\0';
	}

	if (strstr(mode, "MQTT") == NULL) {
		LOG_INF("provision: module mode is '%s', factory flow needed", mode);
		need_full = true;
	}

	if (strcmp(stored_hash, expect_hash) != 0) {
		LOG_INF("provision: parameter fingerprint %s != %s, full rewrite",
			(stored_hash[0] != '\0') ? stored_hash : "none",
			expect_hash);
		need_full = true;
	}

	if (!need_full) {
		cfg_collect_diff(diff, &diff_count);

		if (diff_count == 0U) {
			cfg_state = HOOK_4G_CFG_DONE;
			LOG_INF("provision: module already matches %s (cid=%s)",
				cfg_dev, cfg_cid);
			(void)offline_log_event(
				OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_INF,
				"4g provisioning: module configuration already matches (%s)",
				cfg_dev);
			return 0;
		}
	}

	cfg_state = HOOK_4G_CFG_WRITE;
	/* The module restarts during provisioning: park the heartbeat actions. */
	hook_heartbeat_set_suspend(true);

	if (need_full) {
		(void)offline_log_event(
			OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_WRN,
			"4g provisioning: full rewrite (mode=%s, factory=%u, cid=%s)",
			mode, factory ? 1U : 0U, cfg_cid);
	} else {
		(void)offline_log_event(
			OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_WRN,
			"4g provisioning: writing %u differing item(s) (cid=%s)",
			(unsigned int)diff_count, cfg_cid);
	}

	if (need_full) {
		/* CLEAR wipes the module configuration (and its restart timer),
		 * so it only runs when the module is not usable as it is.
		 */
		hook_4g_modem_boot_reset();
		(void)cfg_at_timeout("AT+CLEAR", resp, sizeof(resp),
				     CFG_CMD_TIMEOUT_MS);
		(void)cfg_wait_modem(CFG_READY_TIMEOUT_MS);
	}

	for (size_t i = 0U; i < cfg_entry_count; i++) {
		if (!need_full && !diff[i]) {
			continue;
		}

		if (cfg_write_entry(&cfg_entries[i]) != 0) {
			hook_heartbeat_set_suspend(false);
			LOG_ERR("provision: writing '%s' failed", cfg_entries[i].cmd);
			return -EIO;
		}
	}

	/* A new configuration only takes effect after save + module restart. */
	hook_4g_modem_boot_reset();
	(void)cfg_at_timeout("AT+S", resp, sizeof(resp), CFG_CMD_TIMEOUT_MS);
	if (cfg_wait_modem(CFG_READY_TIMEOUT_MS) != 0) {
		hook_heartbeat_set_suspend(false);
		LOG_WRN("provision: module did not come back after AT+S");
		return -ETIMEDOUT;
	}

	cfg_state = HOOK_4G_CFG_VERIFY;

	/*
	 * Read back what the module actually stored. The module may restart in
	 * the middle of the rewrite (observed: the banner appeared again while
	 * the entries were being written), in which case the commands sent
	 * during the restart are lost - the MQTT link check alone would still
	 * report success.
	 */
	cfg_collect_diff(diff, &diff_count);
	if (diff_count > 0U) {
		hook_heartbeat_set_suspend(false);
		LOG_WRN("provision: %u item(s) still differ after the rewrite",
			(unsigned int)diff_count);
		(void)offline_log_event(
			OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_WRN,
			"4g provisioning: %u item(s) still differ after rewrite, retrying",
			(unsigned int)diff_count);
		return -EAGAIN;
	}

	ret = cfg_verify_link();
	hook_heartbeat_set_suspend(false);

	if (ret != 0) {
		/* The configuration reads back correctly, so this is a network
		 * problem rather than a configuration one.
		 */
		return -ENOTCONN;
	}

	/*
	 * Remember the parameter set that is now applied to the module.
	 *
	 * device_param_server_save() persists every dirty parameter, so it must
	 * not run while the engineer has an unsaved "param set" pending -
	 * otherwise that change would be stored without a "param save".
	 */
	{
		struct device_param_status param_status;

		device_param_server_get_status(&param_status);

		if (param_status.dirty) {
			LOG_WRN("provision: unsaved parameter change present, "
				"configuration fingerprint not stored");
			(void)offline_log_event(
				OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_WRN,
				"4g provisioning: unsaved parameter change present, "
				"fingerprint not stored (re-provisioned on next boot)");
		} else {
			(void)hook_params_set(CFG_HASH_KEY, expect_hash);
			(void)hook_params_save();
		}
	}

	cfg_state = HOOK_4G_CFG_DONE;
	LOG_INF("provision: configured and connected (cid=%s, dev=%s)", cfg_cid,
		cfg_dev);
	(void)offline_log_event(
		OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_INF,
		"4g provisioning done: cid=%s, topics %s/%s/...", cfg_cid,
		CFG_TOPIC_PREFIX, cfg_dev);

	return 0;
}

static int cfg_run_with_retry(bool factory)
{
	int ret = -EIO;
	bool link_retried = false;

	/*
	 * Wait for the module once, outside the retry budget: a cold module
	 * needs tens of seconds before its AT interpreter answers, and
	 * repeating that wait three times would only delay the start by
	 * minutes. The retries below cover configuration failures instead.
	 */
	cfg_state = HOOK_4G_CFG_WAIT_MODEM;
	if (cfg_wait_modem(CFG_READY_TIMEOUT_MS) != 0) {
		cfg_state = HOOK_4G_CFG_FAILED;
		LOG_ERR("provision: module did not answer within %u ms",
			(unsigned int)CFG_READY_TIMEOUT_MS);
		(void)offline_log_event(
			OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_ERR,
			"4g provisioning: module did not answer within %u ms, retried on next boot",
			(unsigned int)CFG_READY_TIMEOUT_MS);
		return -ETIMEDOUT;
	}

	for (uint32_t attempt = 1U; attempt <= CFG_ATTEMPTS; attempt++) {
		ret = cfg_provision_once(factory);
		if (ret == 0) {
			return 0;
		}

		if (ret == -ENOTCONN) {
			/*
			 * The configuration is stored and reads back correctly,
			 * the module simply has not connected to the broker yet.
			 * Give it one more full cycle (the link may need another
			 * restart), then accept: a missing network is not a
			 * configuration fault and retrying would not help.
			 */
			if (link_retried) {
				cfg_state = HOOK_4G_CFG_DONE;
				LOG_WRN("provision: configuration stored but MQTT not connected");
				(void)offline_log_event(
					OFFLINE_LOG_CAT_NETWORK,
					OFFLINE_LOG_LEVEL_WRN,
					"4g provisioning: configuration stored but MQTT not connected (network?), not retried");
				return 0;
			}

			link_retried = true;
		}

		LOG_WRN("provision attempt %u/%u failed: %d",
			(unsigned int)attempt, (unsigned int)CFG_ATTEMPTS, ret);
		hook_heartbeat_set_suspend(false);

		if (attempt < CFG_ATTEMPTS) {
			k_msleep(CFG_RETRY_DELAY_MS);
		}
	}

	cfg_state = HOOK_4G_CFG_FAILED;
	LOG_ERR("provision failed after %u attempts (last error %d)",
		(unsigned int)CFG_ATTEMPTS, ret);
	(void)offline_log_event(
		OFFLINE_LOG_CAT_NETWORK, OFFLINE_LOG_LEVEL_ERR,
		"4g provisioning failed after %u attempts (last error %d), retried on next boot",
		(unsigned int)CFG_ATTEMPTS, ret);

	return ret;
}

int hook_4g_cfg_run(bool factory)
{
	int ret;

	if (k_mutex_lock(&cfg_mutex, K_MSEC(1000)) != 0) {
		return -EBUSY;
	}

	cfg_build();
	ret = cfg_run_with_retry(factory);

	k_mutex_unlock(&cfg_mutex);

	return ret;
}

enum hook_4g_cfg_state hook_4g_cfg_get_state(void)
{
	return cfg_state;
}

int hook_4g_cfg_enter_httpd(const char *host, uint16_t port, const char *path,
			    uint32_t http_timeout_s)
{
	char cmd[160];
	char resp[256];
	uint32_t start;
	bool addr_ok = false;
	int ret;

	if ((host == NULL) || (host[0] == '\0') || (path == NULL) ||
	    (path[0] == '\0')) {
		return -EINVAL;
	}

	if (k_mutex_lock(&cfg_mutex, K_MSEC(2000)) != 0) {
		return -EBUSY;
	}

	/*
	 * No AT+S has been sent yet in this flow: the module is still running
	 * in MQTT mode, so probe it directly instead of waiting 15 s for a boot
	 * banner that only appears after a restart.
	 */
	if (cfg_wait_modem_wait(CFG_READY_TIMEOUT_MS, false) != 0) {
		ret = -ETIMEDOUT;
		goto out;
	}

	/* HTTPD parameters. The request tail (version + Host + blank line) is
	 * sent by hook_4g_http_trigger() at download time. */
	if (cfg_at("AT+HTPTP=GET", resp, sizeof(resp)) != 0) {
		ret = -EIO;
		goto out;
	}

	(void)snprintk(cmd, sizeof(cmd), "AT+HTPURL=%s", path);
	if (cfg_at(cmd, resp, sizeof(resp)) != 0) {
		ret = -EIO;
		goto out;
	}

	(void)snprintk(cmd, sizeof(cmd), "AT+HTPSV=%s,%u", host,
		       (unsigned int)port);
	if (cfg_at(cmd, resp, sizeof(resp)) != 0) {
		ret = -EIO;
		goto out;
	}

	/*
	 * Keep the response headers: the HTTP OTA session parses Content-Length
	 * from them, which is the only exact statement about how many body bytes
	 * to expect (the MCUboot signature TLV after the image is not a fixed
	 * size, so it cannot be derived from the image header).
	 */
	(void)cfg_at("AT+HTPPK=OFF", resp, sizeof(resp));

	(void)snprintk(cmd, sizeof(cmd), "AT+HTPTIM=%u",
		       (unsigned int)http_timeout_s);
	(void)cfg_at(cmd, resp, sizeof(resp));

	/* Plain HTTP: the URL scheme is not part of AT+HTPURL. */
	(void)cfg_at("AT+SSLEN=OFF", resp, sizeof(resp));

	if (cfg_at("AT+WKMOD=HTTPD", resp, sizeof(resp)) != 0) {
		ret = -EIO;
		goto out;
	}

	hook_4g_modem_boot_reset();
	(void)cfg_at_timeout("AT+S", resp, sizeof(resp), CFG_CMD_TIMEOUT_MS);
	if (cfg_wait_modem(CFG_READY_TIMEOUT_MS) != 0) {
		ret = -ETIMEDOUT;
		goto out;
	}

	if ((cfg_at("AT+WKMOD", resp, sizeof(resp)) != 0) ||
	    (strstr(resp, "HTTPD") == NULL)) {
		LOG_ERR("HTTP OTA: module did not report HTTPD mode");
		ret = -EIO;
		goto out;
	}

	/*
	 * The module only forwards serial data to the server once its network
	 * is up ("wait until the network LED blinks", manual step 9): wait for
	 * an IP address, bounded, so the download can be triggered right away.
	 *
	 * Only a well formed answer counts: an exchange that ends in OK, whose
	 * "+CIP:" value is a real address and not 0.0.0.0. Anything else (an
	 * answer without a terminator, an empty answer, an ERROR) means "not
	 * ready yet" - measured 2026-09-23, the module answered AT+CIP with 24
	 * bytes and no OK/ERROR for two minutes while it was bringing its
	 * network up, and then a download that was triggered on top of that
	 * never received a single byte. Trusting the first line that happens to
	 * contain a dot would have turned that "not ready" into a bogus "ready".
	 *
	 * Polling instead of blocking is what keeps this cheap: the probe gets a
	 * short timeout, and giving up is not fatal because the GET below is
	 * retried - a late network costs one trigger attempt, not the whole
	 * download.
	 */
	start = k_uptime_get_32();
	for (uint32_t i = 0U; i < HOOK_4G_CFG_HTTP_IP_TRIES; i++) {
		char value[64];

		if ((cfg_at_timeout("AT+CIP", resp, sizeof(resp),
				    HOOK_4G_CFG_HTTP_IP_PROBE_MS) == 0) &&
		    cfg_resp_value(resp, value, sizeof(value)) &&
		    (strchr(value, '.') != NULL) &&
		    (strstr(value, "0.0.0.0") == NULL)) {
			LOG_INF("HTTP OTA: module has an IP address (%s)", value);
			addr_ok = true;
			break;
		}

		LOG_DBG("HTTP OTA: no address yet, AT+CIP answered: %s", resp);
		k_msleep(1000);
	}

	if (!addr_ok) {
		uint32_t waited_s = (k_uptime_get_32() - start) / 1000U;

		/*
		 * Do not fail the download over this: the trigger is retried, so
		 * a module that is still attaching gets a second chance. Say it
		 * loudly, because "no address" is also what a dead network looks
		 * like.
		 */
		LOG_WRN("HTTP OTA: no IP address confirmed after %u s, "
			"triggering the GET anyway", waited_s);
		(void)offline_log_event(
			OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
			"HTTP OTA: no IP address confirmed after %u s, triggering the GET anyway",
			waited_s);
	}

	/*
	 * An IP address is not yet a usable link: measured on the bench the
	 * module still needs about 5 s (its network LED keeps blinking) before it
	 * forwards serial data to the server - triggering earlier is silently
	 * dropped.
	 */
	k_msleep(HOOK_4G_CFG_HTTP_NET_SETTLE_MS);

	/* The AT channel answering proves the module is alive and that muting
	 * business traffic did not break the link. */
	if (cfg_at("AT", resp, sizeof(resp)) != 0) {
		LOG_WRN("HTTP OTA: module does not answer AT before the trigger");
	} else {
		LOG_INF("HTTP OTA: module answers AT, ready for the trigger");
	}

	LOG_INF("HTTP OTA: module in HTTPD mode (%s:%u%s)", host,
		(unsigned int)port, path);
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA: module switched to HTTPD (%s:%u%s)",
				host, (unsigned int)port, path);
	ret = 0;

out:
	k_mutex_unlock(&cfg_mutex);
	return ret;
}

int hook_4g_cfg_enter_mqtt(void)
{
	char resp[CFG_RESP_MAX];
	bool diff[CFG_ENTRY_MAX];
	size_t diff_count = 0U;
	int ret;

	if (k_mutex_lock(&cfg_mutex, K_MSEC(2000)) != 0) {
		return -EBUSY;
	}

	/* The module is running (it just answered AT), no banner is coming. */
	if (cfg_wait_modem_wait(CFG_READY_TIMEOUT_MS, false) != 0) {
		ret = -ETIMEDOUT;
		goto out;
	}

	cfg_build();

	/*
	 * Only the work mode has to be put back.
	 *
	 * HTTPD mode does not touch the MQTT side of the module, and nothing in
	 * this flow sends AT+CLEAR (that would reset the module to its factory
	 * defaults and make a full rewrite unavoidable - it is only used by the
	 * factory flow in cfg_provision_once()). So the stored configuration is
	 * read back and only the entries that really differ are written; the
	 * read-back doubles as the verification.
	 */
	cfg_collect_diff(diff, &diff_count);

	for (size_t i = 0U; i < cfg_entry_count; i++) {
		if (!diff[i]) {
			continue;
		}

		if (cfg_write_entry(&cfg_entries[i]) != 0) {
			LOG_ERR("HTTP OTA: writing '%s' on the switch-back failed",
				cfg_entries[i].cmd);
			ret = -EIO;
			goto out;
		}
	}

	LOG_INF("HTTP OTA: switch-back wrote %u of %u configuration item(s)",
		(unsigned int)diff_count, (unsigned int)cfg_entry_count);

	/*
	 * A new work mode only takes effect after save + module restart. When
	 * nothing had to be written (the module already reads back as MQTT, for
	 * example on a retry) there is nothing to save either, and an extra
	 * restart would only delay the recovery.
	 */
	if (diff_count > 0U) {
		hook_4g_modem_boot_reset();
		(void)cfg_at_timeout("AT+S", resp, sizeof(resp), CFG_CMD_TIMEOUT_MS);
		if (cfg_wait_modem(CFG_READY_TIMEOUT_MS) != 0) {
			ret = -ETIMEDOUT;
			goto out;
		}

		/*
		 * Read back once more: the module restarts again shortly after
		 * AT+S, and a command sent during that restart is lost.
		 */
		cfg_collect_diff(diff, &diff_count);
		if (diff_count > 0U) {
			LOG_WRN("HTTP OTA: %u item(s) still differ after the switch-back",
				(unsigned int)diff_count);
			ret = -EAGAIN;
			goto out;
		}
	}

	/*
	 * The module is out of HTTPD mode, so business traffic may flow again.
	 * Unmute before the link check: MQTT frames (a cloud command, the host
	 * heartbeat) that arrive while the module is still connecting would
	 * otherwise be dropped by the HTTPD receive path.
	 */
	hook_4g_set_http_mode(false);

	/*
	 * A configured module is not a connected one: wait until it reports an
	 * established MQTT session. Without this the device could look healthy
	 * while its only management channel is down.
	 */
	for (uint32_t attempt = 0U; attempt < 2U; attempt++) {
		if (cfg_verify_link() == 0) {
			break;
		}

		if (attempt > 0U) {
			/*
			 * The configuration is confirmed, so this is the module's
			 * MQTT client or the network, not the configuration.
			 */
			LOG_WRN("HTTP OTA: module is back in MQTT mode but not connected");
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
				"HTTP OTA: module switched back to MQTT but has no session");
			ret = -ENOTCONN;
			goto out;
		}

		/*
		 * A stale MQTT client is the one cause a module reset fixes, and
		 * only a hard reset is guaranteed: AT+S asks a module that may
		 * not be listening any more. The controller keeps running, so a
		 * staged image survives.
		 */
		LOG_WRN("HTTP OTA: no MQTT session yet, hard-resetting the module once");
		hook_4g_modem_hard_reset();
		if (cfg_wait_modem(CFG_READY_TIMEOUT_MS) != 0) {
			ret = -ETIMEDOUT;
			goto out;
		}
	}

	LOG_INF("HTTP OTA: module switched back to MQTT and connected");
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA: module switched back to MQTT and connected");
	ret = 0;

out:
	k_mutex_unlock(&cfg_mutex);
	return ret;
}

const char *hook_4g_cfg_state_name(enum hook_4g_cfg_state state)
{
	switch (state) {
	case HOOK_4G_CFG_IDLE:
		return "idle";
	case HOOK_4G_CFG_WAIT_MODEM:
		return "wait-modem";
	case HOOK_4G_CFG_READ:
		return "read";
	case HOOK_4G_CFG_WRITE:
		return "write";
	case HOOK_4G_CFG_VERIFY:
		return "verify";
	case HOOK_4G_CFG_DONE:
		return "done";
	case HOOK_4G_CFG_FAILED:
		return "failed";
	default:
		return "?";
	}
}

const char *hook_4g_cfg_cid(void)
{
	return cfg_cid;
}

int hook_4g_cfg_format_summary(char *buf, size_t buf_len)
{
	int written;

	if ((buf == NULL) || (buf_len == 0U)) {
		return -EINVAL;
	}

	if (k_mutex_lock(&cfg_mutex, K_MSEC(1000)) != 0) {
		return -EBUSY;
	}

	cfg_build();
	written = snprintk(buf, buf_len,
			   "dev=%s cid=%s pub=1..3 sub=1..4 svr=%s,%s user=%s",
			   cfg_dev, cfg_cid, hook_params_mqtt_server(),
			   hook_params_mqtt_port(), hook_params_mqtt_user());

	k_mutex_unlock(&cfg_mutex);

	return written;
}

static void hook_4g_cfg_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* Give the 4G channel and the module some time to come up. */
	k_msleep(CFG_BOOT_DELAY_MS);

	(void)hook_4g_cfg_run(false);
}

/* 4 KiB: the flow nests two 768 B answer buffers (86% used at 3 KiB). */
K_THREAD_STACK_DEFINE(hook_4g_cfg_stack, 4096);
static struct k_thread hook_4g_cfg_thread_obj;

void hook_4g_cfg_start(void)
{
	cfg_build();

	k_thread_create(&hook_4g_cfg_thread_obj, hook_4g_cfg_stack,
			K_THREAD_STACK_SIZEOF(hook_4g_cfg_stack),
			hook_4g_cfg_thread, NULL, NULL, NULL,
			OSAL_PRIO_BELOW_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_4g_cfg_thread_obj, "AppCfgTask");
}
