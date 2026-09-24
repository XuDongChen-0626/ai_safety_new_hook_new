/*
 * hook_ota.c - MQTT/4G OTA service (Ymodem -> MCUboot slot 1).
 *
 * Data flow:
 *   host tool --MQTT--> usr.cn --usart1 "2,"--> hook_4g --> hook_ota_feed()
 *   --> Ymodem receiver --> flash_img (slot 1) --> boot_request_upgrade()
 *
 * Responses ('C' / ACK / NAK / CAN and progress text) go back on the same
 * "2," channel, which the usr.cn module routes to the OTA response topic.
 */
#include "hook_ota.h"

#include <string.h>
#include <stdlib.h>

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "hook_health.h"
#include "hook_4g.h"
#include "hook_4g_cfg.h"
#include "hook_heartbeat.h"
#include "hook_params.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "hook_ymodem.h"
#include "device_param_server.h"
#include "offline_log_service.h"
#include "osal_zephyr.h"
#include "ota_manager_service.h"
#include "system_def.h"
#include "uart_manage.h"

LOG_MODULE_REGISTER(hook_ota, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_OTA_CHANNEL "4g"
#define HOOK_OTA_CHANNEL_PREFIX "2,"
#define HOOK_OTA_RX_BLOCK_SIZE 256U
#define HOOK_OTA_RX_QUEUE_DEPTH 16U
#define HOOK_OTA_THREAD_PERIOD_MS 20U
#define HOOK_OTA_PROGRESS_STEP_BYTES 16384U

struct hook_ota_rx_block {
	uint16_t len;
	uint8_t data[HOOK_OTA_RX_BLOCK_SIZE];
};

K_MSGQ_DEFINE(hook_ota_rx_queue, sizeof(struct hook_ota_rx_block),
	      HOOK_OTA_RX_QUEUE_DEPTH, 4);

static hook_ota_state_t hook_ota_state;
static uint32_t hook_ota_rx_drops;
static uint32_t hook_ota_last_activity_ms;
static uint32_t hook_ota_last_progress_bytes;
static uint32_t hook_ota_last_crc_request_ms;
static struct flash_img_context hook_ota_flash_ctx;
static bool hook_ota_flash_active;

static void hook_ota_set_state(hook_ota_state_t state)
{
	if (hook_ota_state != state) {
		LOG_INF("OTA state: %s -> %s", hook_ota_state_name(hook_ota_state),
			hook_ota_state_name(state));
		hook_ota_state = state;
	}
}

const char *hook_ota_state_name(hook_ota_state_t state)
{
	switch (state) {
	case HOOK_OTA_IDLE:
		return "idle";
	case HOOK_OTA_WAIT_START:
		return "wait_start";
	case HOOK_OTA_RECEIVING:
		return "receiving";
	case HOOK_OTA_VERIFYING:
		return "verifying";
	case HOOK_OTA_STAGED:
		return "staged";
	case HOOK_OTA_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

hook_ota_state_t hook_ota_get_state(void)
{
	return hook_ota_state;
}

uint32_t hook_ota_bytes_received(void)
{
	const hook_ymodem_file_info_t *file = hook_ymodem_get_file_info();

	return (file != NULL) ? file->bytes_received : 0U;
}

uint32_t hook_ota_image_size(void)
{
	const hook_ymodem_file_info_t *file = hook_ymodem_get_file_info();

	return (file != NULL) ? file->size : 0U;
}

bool hook_ota_is_active(void)
{
	return (hook_ota_state == HOOK_OTA_WAIT_START) ||
	       (hook_ota_state == HOOK_OTA_RECEIVING) ||
	       (hook_ota_state == HOOK_OTA_VERIFYING);
}

/*
 * OTA channel helpers (usr.cn channel 2 = OTA data / progress).
 */
static int hook_ota_channel_send(const uint8_t *data, uint16_t len)
{
	/*
	 * hook_4g_send_ota() adds the "2," prefix in the same usr.cn frame and
	 * serialises concurrent writes to the 4G link.
	 */
	return hook_4g_send_ota(data, len);
}

static void hook_ota_report(const char *text)
{
	(void)hook_ota_channel_send((const uint8_t *)text, (uint16_t)strlen(text));
}

/*
 * ---------------------------------------------------------------------------
 * Staged image marker (persisted)
 * ---------------------------------------------------------------------------
 *
 * "An image is downloaded and verified, waiting for AT+OTATEST" has to survive
 * a controller reset: a RAM flag cannot, and slot1 alone cannot tell a freshly
 * staged image from the previous one that a swap left behind - both are valid
 * MCUboot images, and installing the old one again is a silent downgrade.
 *
 * So the hash prefix of the staged image is kept in the parameter store
 * ("ota.staged"). It is invalidated whenever slot1 no longer holds that image,
 * when a new download starts writing slot1, and by AT+OTACANCEL / a confirmed
 * install.
 */
#define HOOK_OTA_STAGED_KEY      "ota.staged"
#define HOOK_OTA_STAGED_HASH_HEX 32U

/* Hash prefix of the image in slot1 (false when slot1 holds nothing valid). */
static bool hook_ota_slot1_hash(char *out, size_t out_len)
{
	struct ota_manager_image_info info;

	if ((out == NULL) || (out_len < (HOOK_OTA_STAGED_HASH_HEX + 1U))) {
		return false;
	}

	if ((ota_manager_get_image_info(1U, &info) != 0) || !info.valid) {
		return false;
	}

	memcpy(out, info.hash_hex, HOOK_OTA_STAGED_HASH_HEX);
	out[HOOK_OTA_STAGED_HASH_HEX] = '\0';

	return true;
}

/* Write the marker ("" clears it). Only touches the store when it changes. */
static void hook_ota_staged_marker_write(const char *value)
{
	char current[HOOK_OTA_STAGED_HASH_HEX + 1U];

	if (hook_params_get(HOOK_OTA_STAGED_KEY, current, sizeof(current)) != 0) {
		current[0] = '\0';
	}

	if (strcmp(current, value) == 0) {
		return;
	}

	if (hook_params_set(HOOK_OTA_STAGED_KEY, value) != 0) {
		LOG_ERR("staged marker: could not be updated");
		return;
	}

	/*
	 * The marker is only useful once it is in flash. An unsaved "param set"
	 * of the engineer is persisted together with it - say so rather than
	 * doing it silently.
	 */
	{
		struct device_param_status status;

		device_param_server_get_status(&status);
		if (status.dirty) {
			LOG_WRN("staged marker: an unsaved parameter change is "
				"persisted with it");
		}
	}

	if (hook_params_save() != 0) {
		LOG_ERR("staged marker: could not be saved");
	}
}

/* Remember the image slot1 now holds as "staged". */
static void hook_ota_staged_marker_set(void)
{
	char hash[HOOK_OTA_STAGED_HASH_HEX + 1U];

	if (!hook_ota_slot1_hash(hash, sizeof(hash))) {
		LOG_WRN("staged marker: slot1 holds no valid image");
		return;
	}

	hook_ota_staged_marker_write(hash);
}

static void hook_ota_staged_marker_clear(void)
{
	hook_ota_staged_marker_write("");
}

bool hook_ota_staged_pending(void)
{
	char marker[HOOK_OTA_STAGED_HASH_HEX + 1U];
	char hash[HOOK_OTA_STAGED_HASH_HEX + 1U];

	marker[0] = '\0';
	if (hook_params_get(HOOK_OTA_STAGED_KEY, marker, sizeof(marker)) != 0) {
		return false;
	}

	if (marker[0] == '\0') {
		return false;
	}

	/* The image must still be there, otherwise the marker is stale. */
	if (!hook_ota_slot1_hash(hash, sizeof(hash)) ||
	    (strcmp(marker, hash) != 0)) {
		LOG_WRN("staged marker %s no longer matches slot1", marker);
		return false;
	}

	return true;
}

bool hook_ota_test_available(void)
{
	return (hook_ota_get_state() == HOOK_OTA_STAGED) ||
	       (hook_ota_http_status() == HOOK_OTA_HTTP_STAGED) ||
	       hook_ota_staged_pending();
}

static void hook_ota_report_progress(bool force)
{
	char text[48];
	uint32_t received = hook_ota_bytes_received();
	uint32_t total = hook_ota_image_size();
	int written;

	if (!force &&
	    ((received - hook_ota_last_progress_bytes) <
	     HOOK_OTA_PROGRESS_STEP_BYTES)) {
		return;
	}

	hook_ota_last_progress_bytes = received;

	/* NOTE: the host tool scans response payloads for Ymodem control bytes,
	 * so progress text must not contain an upper case 'C' (0x43).
	 */
	written = snprintf(text, sizeof(text), "PROGRESS,%u,%u", received, total);
	if ((written > 0) && ((size_t)written < sizeof(text))) {
		hook_ota_report(text);
	}
}

/*
 * Ymodem callbacks.
 */
static int hook_ota_ymodem_send(const uint8_t *data, uint16_t len, void *user)
{
	ARG_UNUSED(user);

	return hook_ota_channel_send(data, len);
}

static int hook_ota_ymodem_file_begin(const hook_ymodem_file_info_t *file,
				      void *user)
{
	uint32_t slot_size;
	int ret;

	ARG_UNUSED(user);

	if ((file == NULL) || (file->size_valid == 0U) || (file->size == 0U)) {
		LOG_ERR("OTA image size missing in the Ymodem header");
		return -1;
	}

	slot_size = PARTITION_SIZE(slot1_partition);
	if (file->size > slot_size) {
		LOG_ERR("image %u B does not fit into slot1 (%u B)", file->size,
			slot_size);
		return -1;
	}

	ret = flash_img_init(&hook_ota_flash_ctx);
	if (ret != 0) {
		LOG_ERR("flash_img_init failed: %d", ret);
		return -1;
	}

	hook_ota_flash_active = true;
	hook_ota_last_progress_bytes = 0U;
	hook_ota_set_state(HOOK_OTA_RECEIVING);

	LOG_INF("OTA image '%s' (%u B) -> slot1 (%u B)", file->name, file->size,
		slot_size);
	hook_ota_report_progress(true);

	return 0;
}

static int hook_ota_ymodem_file_data(const uint8_t *data, uint16_t len,
				     uint32_t offset, void *user)
{
	int ret;

	ARG_UNUSED(user);
	ARG_UNUSED(offset);

	if (!hook_ota_flash_active) {
		return -1;
	}

	ret = flash_img_buffered_write(&hook_ota_flash_ctx, data, len, false);
	if (ret != 0) {
		LOG_ERR("flash write failed at %u: %d", offset, ret);
		return -1;
	}

	hook_ota_report_progress(false);

	return 0;
}

static void hook_ota_ymodem_file_end(const hook_ymodem_file_info_t *file,
				     void *user)
{
	struct mcuboot_img_header header;
	uint32_t written;
	size_t expected_min;
	int ret;

	ARG_UNUSED(user);

	hook_ota_set_state(HOOK_OTA_VERIFYING);

	if (!hook_ota_flash_active) {
		LOG_ERR("OTA finished without an open flash context");
		hook_ota_set_state(HOOK_OTA_ERROR);
		hook_ota_report("ERROR,9");
		return;
	}

	/* Flush the remaining buffered block into slot 1. */
	ret = flash_img_buffered_write(&hook_ota_flash_ctx, NULL, 0U, true);
	hook_ota_flash_active = false;
	if (ret != 0) {
		LOG_ERR("flash flush failed: %d", ret);
		hook_ota_set_state(HOOK_OTA_ERROR);
		hook_ota_report("ERROR,10");
		return;
	}

	written = (uint32_t)flash_img_bytes_written(&hook_ota_flash_ctx);
	hook_ota_report_progress(true);

	if ((file != NULL) && (file->size_valid != 0U) && (written < file->size)) {
		LOG_ERR("short write: %u of %u B", written, file->size);
		hook_ota_set_state(HOOK_OTA_ERROR);
		hook_ota_report("ERROR,11");
		return;
	}

	/* Verify that an MCUboot image (not a raw binary) landed in slot 1. */
	memset(&header, 0, sizeof(header));
	ret = boot_read_bank_header(PARTITION_ID(slot1_partition), &header,
				    sizeof(header));
	if (ret != 0) {
		LOG_ERR("slot1 does not hold a valid MCUboot image: %d", ret);
		hook_ota_set_state(HOOK_OTA_ERROR);
		hook_ota_report("ERROR,12");
		return;
	}

	expected_min = (size_t)header.h.v1.image_size + 0x20U;
	if ((header.h.v1.image_size == 0U) ||
	    ((file != NULL) && (file->size_valid != 0U) &&
	     ((size_t)file->size < expected_min))) {
		LOG_ERR("slot1 image header inconsistent (img=%u file=%u)",
			header.h.v1.image_size,
			(file != NULL) ? file->size : 0U);
		hook_ota_set_state(HOOK_OTA_ERROR);
		hook_ota_report("ERROR,13");
		return;
	}

	LOG_INF("slot1 image valid: v%u.%u.%u img=%u B (file %u B)",
		header.h.v1.sem_ver.major, header.h.v1.sem_ver.minor,
		header.h.v1.sem_ver.revision, header.h.v1.image_size,
		(file != NULL) ? file->size : 0U);

	/* Arm the test upgrade: MCUboot swaps on the next reset. */
	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret != 0) {
		LOG_ERR("boot_request_upgrade failed: %d", ret);
		hook_ota_set_state(HOOK_OTA_ERROR);
		hook_ota_report("ERROR,14");
		return;
	}

	hook_health_ota_gate(false);
	hook_ota_set_state(HOOK_OTA_STAGED);

	/*
	 * Persist what was staged: the cloud may install it minutes or hours
	 * later, and the marker is what makes AT+OTATEST survive a reset in
	 * between.
	 */
	hook_ota_staged_marker_set();
	hook_ota_report("DONE");

	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"OTA staged: slot1 image v%u.%u.%u (%u B) verified, "
				"waiting for the test request",
				header.h.v1.sem_ver.major,
				header.h.v1.sem_ver.minor,
				header.h.v1.sem_ver.revision,
				header.h.v1.image_size);

	/*
	 * No automatic reboot: the swap is armed and happens on the next reset,
	 * which is asked for explicitly with AT+OTATEST so the device stays
	 * reachable over MQTT until then.
	 */
	LOG_WRN("OTA staged: slot1 armed, AT+OTATEST installs it");
}

static void hook_ota_ymodem_abort(hook_ymodem_status_t reason, void *user)
{
	ARG_UNUSED(user);

	hook_ota_flash_active = false;
	hook_ota_set_state(HOOK_OTA_ERROR);
	hook_ota_report((reason == HOOK_YMODEM_ERR_ABORTED) ? "ERROR,20" :
							      "ERROR,21");
	hook_health_ota_gate(false);

	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
				"OTA aborted (reason=%d), staying on the running image",
				(int)reason);

	LOG_ERR("OTA aborted (reason=%d), staying on the running image",
		(int)reason);
}

static const hook_ymodem_config_t hook_ota_ymodem_config = {
	.send = hook_ota_ymodem_send,
	.on_file_begin = hook_ota_ymodem_file_begin,
	.on_file_data = hook_ota_ymodem_file_data,
	.on_file_end = hook_ota_ymodem_file_end,
	.on_abort = hook_ota_ymodem_abort,
	.user = NULL,
	.max_errors = CONFIG_HOOK_OTA_MAX_ERRORS,
	.packet_timeout_ms = CONFIG_HOOK_OTA_PACKET_TIMEOUT_MS,
};

/*
 * ---------------------------------------------------------------------------
 * HTTP download session (4G module in HTTPD mode)
 * ---------------------------------------------------------------------------
 *
 * The module streams the HTTP response head and body over the serial line
 * (AT+HTPPK=OFF keeps the head). The head carries Content-Length, which is the
 * exact body size - including the MCUboot signature TLV that follows the
 * image, which is not a fixed size and cannot be derived from the image
 * header. Slot1 is opened as soon as the size is known.
 */
/* Response head buffer (this server sends ~700 B with its CORS headers). */
#define HOOK_OTA_HTTP_HEAD_MAX 1024U
/* Response head, parsed once for the status code and Content-Length. */
static uint8_t hook_ota_http_head[HOOK_OTA_HTTP_HEAD_MAX];
static uint16_t hook_ota_http_head_len;
static bool hook_ota_http_head_done;
/* Body length as declared by the server (Content-Length). */
static uint32_t hook_ota_http_expected;
static uint32_t hook_ota_http_received;
static volatile bool hook_ota_http_armed;

void hook_ota_http_reset(void)
{
	hook_ota_http_head_len = 0U;
	hook_ota_http_head_done = false;
	hook_ota_http_expected = 0U;
	hook_ota_http_received = 0U;
	hook_ota_http_armed = false;
	hook_ota_flash_active = false;
}

void hook_ota_http_arm(void)
{
	hook_ota_http_armed = true;
}

bool hook_ota_http_is_armed(void)
{
	return hook_ota_http_armed;
}

uint32_t hook_ota_http_bytes(void)
{
	return hook_ota_http_received;
}

uint32_t hook_ota_http_expect_bytes(void)
{
	return hook_ota_http_expected;
}

/*
 * Parse the response head: require 200, take Content-Length as the exact body
 * size and refuse anything the downloader cannot handle.
 */
static int hook_ota_http_parse_head(const char *head, uint32_t *body_len)
{
	const char *p;
	const char *cl;
	long status;
	unsigned long len;

	if (strncmp(head, "HTTP/", 5U) != 0) {
		LOG_ERR("HTTP OTA: response does not start with a status line");
		return -EBADMSG;
	}

	p = strchr(head, ' ');
	if (p == NULL) {
		return -EBADMSG;
	}

	status = strtol(p + 1, NULL, 10);
	if (status != 200) {
		LOG_ERR("HTTP OTA: server answered %ld, expected 200", status);
		return -EIO;
	}

	if (strstr(head, "chunked") != NULL) {
		LOG_ERR("HTTP OTA: chunked responses are not supported");
		return -ENOTSUP;
	}

	cl = strstr(head, "Content-Length:");
	if (cl == NULL) {
		LOG_ERR("HTTP OTA: no Content-Length in the response");
		return -EBADMSG;
	}

	len = strtoul(cl + strlen("Content-Length:"), NULL, 10);
	if (len == 0UL) {
		LOG_ERR("HTTP OTA: empty body advertised");
		return -EBADMSG;
	}

	*body_len = (uint32_t)len;

	return 0;
}

/* Open slot1 for a body of the given length. */
static int hook_ota_http_open_slot(uint32_t body_len)
{
	uint32_t slot_size = PARTITION_SIZE(slot1_partition);
	int ret;

	if ((body_len == 0U) || (body_len > slot_size)) {
		LOG_ERR("HTTP OTA: body of %u B does not fit slot1 (%u B)",
			body_len, slot_size);
		return -EINVAL;
	}

	ret = flash_img_init(&hook_ota_flash_ctx);
	if (ret != 0) {
		LOG_ERR("HTTP OTA: flash_img_init failed: %d", ret);
		return -EIO;
	}

	hook_ota_flash_active = true;
	hook_ota_http_expected = body_len;
	hook_ota_set_state(HOOK_OTA_RECEIVING);

	/*
	 * Slot1 is about to be overwritten, so whatever was staged before is
	 * gone - do not let a stale marker survive into the new session.
	 */
	hook_ota_staged_marker_clear();

	LOG_INF("HTTP OTA: slot1 open, expecting %u B (from Content-Length)",
		body_len);
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA: response head accepted, body %u B",
				body_len);

	return 0;
}

/* Flush slot1, verify the image and arm the MCUboot test upgrade. */
int hook_ota_http_finish(void)
{
	struct mcuboot_img_header header;
	uint32_t written;
	int ret;

	hook_ota_set_state(HOOK_OTA_VERIFYING);

	/* The response is complete: stop feeding the body into the session. */
	hook_ota_http_armed = false;

	ret = flash_img_buffered_write(&hook_ota_flash_ctx, NULL, 0U, true);
	hook_ota_flash_active = false;
	if (ret != 0) {
		LOG_ERR("HTTP OTA: flash flush failed: %d", ret);
		goto error;
	}

	written = (uint32_t)flash_img_bytes_written(&hook_ota_flash_ctx);
	if (written < hook_ota_http_expected) {
		LOG_ERR("HTTP OTA: short write %u of %u B", written,
			hook_ota_http_expected);
		goto error;
	}

	memset(&header, 0, sizeof(header));
	ret = boot_read_bank_header(PARTITION_ID(slot1_partition), &header,
				    sizeof(header));
	if (ret != 0) {
		LOG_ERR("HTTP OTA: slot1 does not hold a valid MCUboot image: %d",
			ret);
		goto error;
	}

	LOG_INF("HTTP OTA: slot1 image valid: v%u.%u.%u img=%u B",
		header.h.v1.sem_ver.major, header.h.v1.sem_ver.minor,
		header.h.v1.sem_ver.revision, header.h.v1.image_size);

	hook_health_ota_gate(false);
	hook_ota_set_state(HOOK_OTA_STAGED);

	/*
	 * Deliberately NOT boot_request_upgrade() here: the device stays on the
	 * running image (and therefore reachable over MQTT) until the cloud asks
	 * for the test, see hook_ota_test_and_reboot().
	 */
	LOG_INF("HTTP OTA ready: slot1 image v%u.%u.%u (%u B) verified, "
		"waiting for the test request",
		header.h.v1.sem_ver.major, header.h.v1.sem_ver.minor,
		header.h.v1.sem_ver.revision, header.h.v1.image_size);
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA ready: slot1 image v%u.%u.%u (%u B) verified, "
				"waiting for the test request",
				header.h.v1.sem_ver.major,
				header.h.v1.sem_ver.minor,
				header.h.v1.sem_ver.revision,
				header.h.v1.image_size);
	return 1;

error:
	hook_ota_set_state(HOOK_OTA_ERROR);
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
				"HTTP OTA failed after %u B, staying on the running image",
				hook_ota_http_received);
	return -EIO;
}

int hook_ota_http_feed(const uint8_t *data, uint16_t len)
{
	uint16_t copy;
	int ret;

	if ((data == NULL) || (len == 0U)) {
		return 0;
	}

	/*
	 * Phase 0: response head. It carries Content-Length, which is the only
	 * exact statement about the body size - the MCUboot signature TLV that
	 * follows the image data is not a fixed size, so it cannot be derived
	 * from the image header.
	 */
	if (!hook_ota_http_head_done) {
		uint16_t space = (uint16_t)(sizeof(hook_ota_http_head) - 1U -
					    hook_ota_http_head_len);
		uint16_t body_start = 0U;

		copy = (len < space) ? len : space;
		if (copy > 0U) {
			memcpy(&hook_ota_http_head[hook_ota_http_head_len], data,
			       copy);
			hook_ota_http_head_len =
				(uint16_t)(hook_ota_http_head_len + copy);
			data += copy;
			len = (uint16_t)(len - copy);
		}

		/* The head ends with an empty line. */
		for (uint16_t i = 3U; i < hook_ota_http_head_len; i++) {
			if ((hook_ota_http_head[i - 3U] == '\r') &&
			    (hook_ota_http_head[i - 2U] == '\n') &&
			    (hook_ota_http_head[i - 1U] == '\r') &&
			    (hook_ota_http_head[i] == '\n')) {
				body_start = (uint16_t)(i + 1U);
				break;
			}
		}

		if (body_start == 0U) {
			if (hook_ota_http_head_len >=
			    (sizeof(hook_ota_http_head) - 1U)) {
				LOG_ERR("HTTP OTA: response head too long");
				goto fail;
			}
			return 0;
		}

		hook_ota_http_head[hook_ota_http_head_len] = '\0';

		ret = hook_ota_http_parse_head((const char *)hook_ota_http_head,
					       &hook_ota_http_expected);
		if (ret != 0) {
			goto fail;
		}

		ret = hook_ota_http_open_slot(hook_ota_http_expected);
		if (ret != 0) {
			goto fail;
		}

		hook_ota_http_head_done = true;

		/* The body may already have started inside the same chunk. */
		if (hook_ota_http_head_len > body_start) {
			uint16_t extra =
				(uint16_t)(hook_ota_http_head_len - body_start);

			ret = flash_img_buffered_write(
				&hook_ota_flash_ctx,
				&hook_ota_http_head[body_start], extra, false);
			if (ret != 0) {
				LOG_ERR("HTTP OTA: flash write failed: %d", ret);
				goto fail;
			}
			hook_ota_http_received += extra;
		}

		if (len == 0U) {
			return (hook_ota_http_received >=
				hook_ota_http_expected) ? 1 : 0;
		}
	}

	/* Phase 1: the body itself (image + signature TLV). */
	if (hook_ota_http_received < hook_ota_http_expected) {
		uint32_t remaining =
			hook_ota_http_expected - hook_ota_http_received;

		if ((uint32_t)len > remaining) {
			len = (uint16_t)remaining;
		}

		ret = flash_img_buffered_write(&hook_ota_flash_ctx, data, len,
					       false);
		if (ret != 0) {
			LOG_ERR("HTTP OTA: flash write failed at %u: %d",
				hook_ota_http_received, ret);
			goto fail;
		}

		hook_ota_http_received += len;
	}

	return (hook_ota_http_received >= hook_ota_http_expected) ? 1 : 0;

fail:
	/* Slot1 now holds a partial image: forget the staged marker. */
	hook_ota_staged_marker_clear();
	hook_ota_http_reset();
	hook_ota_set_state(HOOK_OTA_ERROR);
	return -EIO;
}

/*
 * ---------------------------------------------------------------------------
 * HTTP OTA orchestration
 * ---------------------------------------------------------------------------
 *
 * Triggered by "craner#AT+OTADL=<url>" (topic 1). The
 * whole sequence takes tens of seconds, so it runs on its own thread:
 *
 *   mute business TX -> switch the module to HTTPD -> trigger the GET
 *   -> stream the image into slot1 -> switch the module back to MQTT -> unmute
 *
 * The "switch back" step runs on every path, failures included: leaving the
 * module in HTTPD mode would cost the device its MQTT link.
 */
#define HOOK_OTA_HTTP_URL_MAX  208U
#define HOOK_OTA_HTTP_HOST_MAX 64U
#define HOOK_OTA_HTTP_PATH_MAX 200U
/* Upper bound for one download (280 KB at serial line speed takes ~25 s). */
#define HOOK_OTA_HTTP_MAX_MS (10U * 60U * 1000U)
/* Module side HTTP timeout (AT+HTPTIM). */
#define HOOK_OTA_HTTP_MODEM_TIMEOUT_S 120U
/*
 * How long one trigger is given before it is sent again.
 *
 * Measured on the bench: the response started ~26 s after the trigger, so a
 * short window made the flow re-trigger while the first request was still on
 * its way - three triggers could then be concatenated into one garbage
 * request. Fewer triggers with a longer window work better.
 */
#define HOOK_OTA_HTTP_TRIGGER_WAIT_MS 40000U
#define HOOK_OTA_HTTP_TRIGGER_TRIES   2U
/*
 * Guard between queueing the download and muting the business TX path: the AT
 * handler that requested it (craner#AT+OTADL) publishes its "craner#OK" as soon
 * as hook_ota_http_request() returns, so the mute must not come first.
 */
#define HOOK_OTA_HTTP_TX_SETTLE_MS    300U

static char hook_ota_http_url[HOOK_OTA_HTTP_URL_MAX];
static volatile bool hook_ota_http_requested;
static volatile uint8_t hook_ota_http_state_value;
K_SEM_DEFINE(hook_ota_http_signal, 0, 1);

/*
 * Publish the download result on the OTA topic (channel 2 ->
 * ai_safety/<dev>/ota/hook/server).
 *
 * The module is back in MQTT mode by then, and the cloud needs this to decide
 * whether to send AT+OTATEST. The payload stays short (hook_4g_send_ota() takes
 * at most 62 bytes) and uses the "+..." style of the AT answers:
 *
 *   +OTADL:OK,<body bytes>,<image bytes>,v<version>,<hash prefix>
 *   +OTADL:ERR,<reason>,<body bytes>,<expected bytes>
 *
 * reason: 1 URL rejected, 2 module setup failed, 3 no response data,
 *         4 receive/write/verify failed.
 */
static void hook_ota_publish_result(uint8_t reason)
{
	struct ota_manager_image_info info;
	char line[64];
	int written;

	if (reason == 0U) {
		if ((ota_manager_get_image_info(1U, &info) == 0) && info.valid) {
			written = snprintf(line, sizeof(line), "+OTADL:OK,%u,%u,%s,%.8s",
					   hook_ota_http_bytes(), info.image_size,
					   info.version, info.hash_hex);
		} else {
			written = snprintf(line, sizeof(line), "+OTADL:OK,%u,0,-,-",
					   hook_ota_http_bytes());
		}
	} else {
		written = snprintf(line, sizeof(line), "+OTADL:ERR,%u,%u,%u",
				   (unsigned int)reason, hook_ota_http_bytes(),
				   hook_ota_http_expect_bytes());
	}

	if ((written <= 0) || ((size_t)written >= sizeof(line))) {
		LOG_ERR("OTA result message too long");
		return;
	}

	/*
	 * Publish ONCE, on channel 2 (ai_safety/<dev>/ota/hook/server) - the
	 * topic that exists for exactly this: OTA progress and results. (The
	 * Ymodem progress messages go there as well.)
	 *
	 * One channel only, on purpose: the module merges serial frames that
	 * follow each other without a gap (20 ms packet timeout) into ONE MQTT
	 * message and honours only the first channel prefix. Publishing the
	 * same result on channel 1 and channel 2 back to back made the cloud
	 * receive
	 *
	 *   craner#+OTADL:OK,252256,250896,2.1.0+0,7539b8142,+OTADL:OK,...
	 *
	 * i.e. both payloads glued together with a stray "2," inside (seen
	 * 2026-09-24 18:01).
	 *
	 * The cloud has to subscribe to this topic to see the result; the
	 * command answers stay on channel 1 as before.
	 */
	if (hook_ota_channel_send((const uint8_t *)line, (uint16_t)written) != 0) {
		LOG_WRN("OTA result not published on the OTA topic: %s", line);
		return;
	}

	LOG_INF("OTA result published on the OTA topic: %s", line);
}

uint8_t hook_ota_http_status(void)
{
	return hook_ota_http_state_value;
}

int hook_ota_http_request(const char *url, uint32_t len)
{
	if ((url == NULL) || (len == 0U) || (len >= sizeof(hook_ota_http_url))) {
		return -EINVAL;
	}

	if (hook_ota_http_requested ||
	    (hook_ota_http_state_value == HOOK_OTA_HTTP_DOWNLOADING)) {
		return -EBUSY;
	}

	memcpy(hook_ota_http_url, url, len);
	hook_ota_http_url[len] = '\0';
	hook_ota_http_requested = true;
	k_sem_give(&hook_ota_http_signal);

	return 0;
}

/* Split "http://host[:port]/path"; the scheme is optional. */
static int hook_ota_http_parse(const char *url, char *host, size_t host_len,
			       uint16_t *port, char *path, size_t path_len)
{
	const char *p = url;
	const char *slash;
	const char *colon;
	size_t used;

	if (strncmp(p, "http://", 7U) == 0) {
		p += 7U;
	}

	slash = strchr(p, '/');
	if (slash == NULL) {
		return -EINVAL;
	}

	colon = memchr(p, ':', (size_t)(slash - p));
	used = (size_t)((colon != NULL) ? (colon - p) : (slash - p));

	if ((used == 0U) || (used >= host_len)) {
		return -EINVAL;
	}

	memcpy(host, p, used);
	host[used] = '\0';

	*port = 80U;
	if (colon != NULL) {
		long value = strtol(colon + 1, NULL, 10);

		if ((value <= 0) || (value > 65535)) {
			return -EINVAL;
		}
		*port = (uint16_t)value;
	}

	if (strlen(slash) >= path_len) {
		return -EINVAL;
	}
	strcpy(path, slash);

	return 0;
}

static void hook_ota_http_run(void)
{
	char host[HOOK_OTA_HTTP_HOST_MAX];
	char path[HOOK_OTA_HTTP_PATH_MAX];
	uint16_t port = 80U;
	uint32_t start;
	uint8_t fail_reason = 0U;
	int ret;

	hook_ota_http_requested = false;

	ret = hook_ota_http_parse(hook_ota_http_url, host, sizeof(host), &port,
				  path, sizeof(path));
	if (ret != 0) {
		hook_ota_http_state_value = HOOK_OTA_HTTP_ERROR;
		LOG_ERR("HTTP OTA: malformed URL");
		(void)offline_log_event(OFFLINE_LOG_CAT_OTA,
					OFFLINE_LOG_LEVEL_ERR,
					"HTTP OTA rejected: malformed URL");
		hook_ota_publish_result(1U);
		return;
	}

	LOG_INF("HTTP OTA: downloading %s:%u%s", host, (unsigned int)port, path);
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA start: %s:%u%s", host,
				(unsigned int)port, path);

	hook_ota_http_state_value = HOOK_OTA_HTTP_DOWNLOADING;
	hook_ota_http_reset();

	/* Let the "craner#OK" of the requesting command reach the module. */
	k_msleep(HOOK_OTA_HTTP_TX_SETTLE_MS);

	/* Mute business TX first: in HTTPD mode anything sent to the module is
	 * appended to the HTTP request being built. */
	hook_4g_set_http_mode(true);
	hook_heartbeat_set_suspend(true);

	/*
	 * Stop the task check-in supervision for the whole session: the 4G
	 * thread blocks for seconds inside the slot1 sector erases, and the
	 * module restarts two times on the way in and out. Pausing the
	 * supervision is the module-level way to do that, see system_def.h.
	 */
	hook_health_ota_gate(true);

	ret = hook_4g_cfg_enter_httpd(host, port, path,
				      HOOK_OTA_HTTP_MODEM_TIMEOUT_S);

	if (ret == 0) {
		/*
		 * The module only forwards the trigger to the server once it is
		 * online, and it silently drops it otherwise, so retry the
		 * request until data starts arriving.
		 */
		for (uint32_t attempt = 1U;
		     attempt <= HOOK_OTA_HTTP_TRIGGER_TRIES; attempt++) {
			if (hook_ota_http_bytes() > 0U) {
				break;
			}

			LOG_INF("HTTP OTA: sending GET (attempt %u)", attempt);
			ret = hook_4g_http_trigger(host, port);
			if (ret != 0) {
				break;
			}
			/* From here on the response body may arrive. */
			hook_ota_http_arm();

			start = k_uptime_get_32();
			while ((uint32_t)(k_uptime_get_32() - start) <
			       HOOK_OTA_HTTP_TRIGGER_WAIT_MS) {
				if (hook_ota_http_bytes() > 0U) {
					break;
				}
				k_msleep(200);
			}
		}

		if (hook_ota_http_bytes() == 0U) {
			LOG_ERR("HTTP OTA: no data after %u trigger(s)",
				(unsigned int)HOOK_OTA_HTTP_TRIGGER_TRIES);
			fail_reason = 3U;
		} else {
			/*
			 * Wait for the whole body: Content-Length gives the exact
			 * size, so the session ends as soon as that many bytes
			 * have been written.
			 */
			start = k_uptime_get_32();
			while ((uint32_t)(k_uptime_get_32() - start) <
			       HOOK_OTA_HTTP_MAX_MS) {
				hook_ota_state_t st = hook_ota_get_state();
				uint32_t expect = hook_ota_http_expect_bytes();

				if ((st == HOOK_OTA_STAGED) ||
				    (st == HOOK_OTA_ERROR)) {
					break;
				}

				/*
				 * Content-Length is exact, so the body is complete
				 * the moment that many bytes have been written.
				 */
				if ((expect > 0U) &&
				    (hook_ota_http_bytes() >= expect)) {
					LOG_INF("HTTP OTA: body complete (%u B)",
						hook_ota_http_bytes());
					(void)hook_ota_http_finish();
					break;
				}

				k_msleep(100);
			}
		}
	} else {
		LOG_ERR("HTTP OTA: module setup failed: %d", ret);
		fail_reason = 2U;
	}

	/*
	 * Always give the MQTT link back. This must not be "best effort": an
	 * unreachable device has lost its only management channel, so a second
	 * failure reboots the controller - the boot time provisioning flow puts
	 * the module back into MQTT mode whenever it is not in it.
	 */
	ret = hook_4g_cfg_enter_mqtt();

	/*
	 * -ENOTCONN is different: the module is in MQTT mode with a verified
	 * configuration but has no session, which is a network or server problem.
	 * A controller reboot would not reset the module, would not help the
	 * network, and would throw away the image that is already staged - so
	 * keep running and report it (the module retries on its own).
	 */
	if (ret == -ENOTCONN) {
		LOG_ERR("HTTP OTA: MQTT session did not come up, staged image kept");
		(void)offline_log_event(
			OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
			"HTTP OTA: module back in MQTT mode without a session, staged image kept");
	} else if (ret != 0) {
		LOG_ERR("HTTP OTA: MQTT restore failed (%d), retrying", ret);
		k_msleep(2000);
		ret = hook_4g_cfg_enter_mqtt();

		if (ret != 0) {
			LOG_ERR("HTTP OTA: MQTT restore failed twice, rebooting to recover");
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
				"HTTP OTA: MQTT restore failed twice, rebooting to recover");
			hook_4g_set_http_mode(false);
			k_msleep(200);
			sys_reboot(SYS_REBOOT_COLD);
		}
	}

	hook_4g_set_http_mode(false);
	hook_heartbeat_set_suspend(false);
	hook_health_ota_gate(false);

	if (hook_ota_get_state() == HOOK_OTA_STAGED) {
		hook_ota_http_state_value = HOOK_OTA_HTTP_STAGED;

		/*
		 * The image is staged either way and stays usable; only the MQTT
		 * session may still be pending, so say which one it is.
		 */
		if (ret == -ENOTCONN) {
			LOG_WRN("HTTP OTA: complete, %u B written to slot1, "
				"MQTT session not up yet",
				hook_ota_http_bytes());
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
				"HTTP OTA ready, waiting for the test request (MQTT session pending)");
		} else {
			LOG_INF("HTTP OTA: complete, %u B written to slot1, MQTT restored",
				hook_ota_http_bytes());
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA ready, MQTT restored, waiting for the test request");
		}

		/* Tell the cloud it can ask for the test. */
		hook_ota_publish_result(0U);
		return;
	}

	hook_ota_http_state_value = HOOK_OTA_HTTP_ERROR;
	LOG_ERR("HTTP OTA: failed (state=%s, %u/%u B)",
		hook_ota_state_name(hook_ota_get_state()),
		hook_ota_http_bytes(), hook_ota_http_expect_bytes());
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
				"HTTP OTA failed (%u/%u B), MQTT restored",
				hook_ota_http_bytes(),
				hook_ota_http_expect_bytes());

	/*
	 * 4 = receive/write/verify failed; 2/3 were set by the step that failed.
	 */
	hook_ota_publish_result((fail_reason != 0U) ? fail_reason : 4U);
}

static void hook_ota_http_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		(void)k_sem_take(&hook_ota_http_signal, K_FOREVER);
		hook_ota_http_run();
	}
}

K_THREAD_STACK_DEFINE(hook_ota_http_stack, 4096);
static struct k_thread hook_ota_http_thread_obj;

void hook_ota_init(void)
{
	hook_ymodem_init(&hook_ota_ymodem_config);
	hook_ota_state = HOOK_OTA_IDLE;
	hook_ota_http_state_value = HOOK_OTA_HTTP_IDLE;

	/*
	 * A download may have finished before this reset, in which case only
	 * the persisted marker (ota.staged) knows about it - say so, otherwise
	 * the image in slot1 looks like a leftover nobody asked about.
	 */
	if (hook_ota_staged_pending()) {
		LOG_WRN("slot1 holds a staged image from an earlier download "
			"(ota.staged), craner#AT+OTATEST installs it");
		(void)offline_log_event(
			OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
			"slot1 holds a staged image from an earlier download "
			"(ota.staged), waiting for AT+OTATEST");
	} else {
		/*
		 * Slot1 moved on (a confirmed install, a rollback, a cancelled
		 * session) or holds nothing: drop the marker so it cannot be
		 * taken for a ready image later. No store write when it is
		 * already empty.
		 */
		hook_ota_staged_marker_clear();
	}
}

int hook_ota_request(void)
{
	int ret;

	if (hook_ota_is_active()) {
		/* The host tool repeats the start command every 10 s: re-arm the
		 * CRC request instead of restarting the session.
		 */
		LOG_INF("OTA already active (%s), re-sending 'C'",
			hook_ota_state_name(hook_ota_state));
		return (int)hook_ymodem_send_control(HOOK_YMODEM_CRC_REQUEST);
	}

	LOG_INF("OTA session requested");
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"OTA receiver armed by the host tool");
	hook_ota_last_progress_bytes = 0U;
	hook_ota_last_crc_request_ms = 0U;
	hook_ota_last_activity_ms = k_uptime_get_32();
	hook_ota_flash_active = false;
	hook_ota_rx_drops = 0U;

	/* Drop bytes that arrived before the session was armed. */
	k_msgq_purge(&hook_ota_rx_queue);

	hook_health_ota_gate(true);
	hook_ota_set_state(HOOK_OTA_WAIT_START);

	ret = hook_ymodem_start_receive();
	if (ret != HOOK_YMODEM_OK) {
		LOG_ERR("could not start the Ymodem receiver: %d", ret);
		hook_health_ota_gate(false);
		hook_ota_set_state(HOOK_OTA_ERROR);
		return ret;
	}

	hook_ota_last_crc_request_ms = k_uptime_get_32();

	return 0;
}

void hook_ota_cancel(void)
{
	/*
	 * A cancelled session leaves slot1 in an unknown state (it may have
	 * been written half way), so a previously staged image must not stay
	 * installable.
	 */
	hook_ota_staged_marker_clear();

	if (!hook_ota_is_active()) {
		LOG_INF("OTA cancel: no session active");
		hook_ymodem_reset();
		hook_ota_set_state(HOOK_OTA_IDLE);
		return;
	}

	LOG_WRN("OTA cancel requested");
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
				"OTA session cancelled");

	/* Tell a possibly still transmitting host to stop (CAN CAN). */
	{
		static const uint8_t cancel[2] = {HOOK_YMODEM_CAN, HOOK_YMODEM_CAN};

		(void)hook_ota_channel_send(cancel, (uint16_t)sizeof(cancel));
	}

	hook_ymodem_reset();
	hook_ota_flash_active = false;
	hook_health_ota_gate(false);
	hook_ota_set_state(HOOK_OTA_IDLE);
}

int hook_ota_confirm(void)
{
	int ret = ota_manager_confirm_current_image();

	if (ret != 0) {
		(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
					"image confirm failed (%d)", ret);
		return ret;
	}

	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"running image confirmed by the engineer");

	/*
	 * The image is installed and accepted: nothing is waiting for a test
	 * any more. (Whatever slot1 holds now is the previous image - staging
	 * that one again would be a downgrade.)
	 */
	hook_ota_staged_marker_clear();

	return 0;
}

int hook_ota_test_and_reboot(void)
{
	bool staged_earlier = false;
	int ret;

	if (!hook_ota_test_available()) {
		/* Drop a marker that no longer describes slot1. */
		hook_ota_staged_marker_clear();
		LOG_WRN("OTA test requested but no image is staged (state=%s)",
			hook_ota_state_name(hook_ota_state));
		return -EINVAL;
	}

	/*
	 * The image may have been downloaded before the last reset: then only
	 * the persisted marker knows about it (see ota.staged).
	 */
	staged_earlier = (hook_ota_state != HOOK_OTA_STAGED) &&
			 (hook_ota_http_status() != HOOK_OTA_HTTP_STAGED);
	if (staged_earlier) {
		LOG_WRN("OTA test requested for the image staged before the "
			"last reset (ota.staged)");
		(void)offline_log_event(
			OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
			"OTA test requested for an image staged before the last reset");
	}

	/*
	 * Arm the test upgrade: MCUboot swaps slot0/slot1 on the next reset and
	 * rolls back if the new image is not confirmed within the window of
	 * ota_manager_service.
	 */
	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret != 0) {
		LOG_ERR("boot_request_upgrade failed: %d", ret);
		return -EIO;
	}

	LOG_WRN("OTA test requested: rebooting in %u ms for MCUboot",
		(unsigned int)CONFIG_HOOK_OTA_REBOOT_DELAY_MS);
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
				"OTA test requested, rebooting into the MCUboot test image");

	k_msleep(CONFIG_HOOK_OTA_REBOOT_DELAY_MS);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

void hook_ota_feed(const uint8_t *data, uint16_t len)
{
	struct hook_ota_rx_block block;
	uint16_t offset = 0U;

	if ((data == NULL) || (len == 0U)) {
		return;
	}

	/* Split a frame of up to 2 KiB into queue blocks. */
	while (offset < len) {
		uint16_t chunk = (uint16_t)(len - offset);

		if (chunk > HOOK_OTA_RX_BLOCK_SIZE) {
			chunk = HOOK_OTA_RX_BLOCK_SIZE;
		}

		block.len = chunk;
		memcpy(block.data, &data[offset], chunk);

		if (k_msgq_put(&hook_ota_rx_queue, &block, K_NO_WAIT) != 0) {
			hook_ota_rx_drops++;
		}

		offset = (uint16_t)(offset + chunk);
	}
}

/*
 * The host tool waits for the receiver to ask for data. Re-arm the CRC
 * request periodically while the receiver has no complete packet, otherwise
 * the tool stalls on every "waiting for 'C'" step until its 30 s timeout.
 */
static void hook_ota_keepalive_c(void)
{
	uint32_t now = k_uptime_get_32();

	if (!hook_ota_is_active()) {
		return;
	}

	/* Never drive the Ymodem 'C' handshake into an HTTP download: the
	 * module would append it to the HTTP request under construction.
	 */
	if (hook_ota_http_status() == HOOK_OTA_HTTP_DOWNLOADING) {
		return;
	}

	if ((now - hook_ota_last_crc_request_ms) <
	    CONFIG_HOOK_OTA_CRC_REQUEST_INTERVAL_MS) {
		return;
	}

	hook_ota_last_crc_request_ms = now;

	if (hook_ymodem_waiting_for_packet()) {
		(void)hook_ymodem_send_control(HOOK_YMODEM_CRC_REQUEST);
	}
}

static void hook_ota_session_timeout_check(void)
{
	if (!hook_ota_is_active()) {
		return;
	}

	/*
	 * The HTTP download shares hook_ota_state with the Ymodem session, but
	 * not the activity stamp: hook_ota_last_activity_ms is only written by
	 * the Ymodem path, so once the controller has been up for
	 * CONFIG_HOOK_OTA_SESSION_TIMEOUT_MS this check aborts every HTTP
	 * download a moment after it enters HOOK_OTA_RECEIVING (observed
	 * 2026-09-24, 29 minutes of uptime: "OTA session timeout (600000 ms
	 * without data), aborting" 2.5 s after slot1 was opened). The HTTP
	 * session has its own timeouts, see hook_ota_http_run().
	 */
	if (hook_ota_http_status() == HOOK_OTA_HTTP_DOWNLOADING) {
		return;
	}

	if ((k_uptime_get_32() - hook_ota_last_activity_ms) <
	    CONFIG_HOOK_OTA_SESSION_TIMEOUT_MS) {
		return;
	}

	LOG_ERR("OTA session timeout (%u ms without data), aborting",
		(unsigned int)CONFIG_HOOK_OTA_SESSION_TIMEOUT_MS);
	hook_ota_report("ERROR,22");
	hook_ota_cancel();
}

/*
 * There is deliberately no automatic reboot when an image is staged: the
 * Ymodem and the HTTP download share hook_ota_state, and the old helper that
 * rebooted on HOOK_OTA_STAGED fired the instant the HTTP body was complete -
 * before the module had been switched back to MQTT. The reboot now happens
 * only when the test upgrade is asked for explicitly (AT+OTATEST ->
 * hook_ota_test_and_reboot()), so the device stays reachable in between.
 */
static void hook_ota_thread(void *a, void *b, void *c)
{
	(void)a;
	(void)b;
	(void)c;

	for (;;) {
		struct hook_ota_rx_block block;

		if (k_msgq_get(&hook_ota_rx_queue, &block, K_MSEC(5)) == 0) {
			hook_ota_last_activity_ms = k_uptime_get_32();

			for (uint16_t i = 0U; i < block.len; i++) {
				hook_ymodem_input_byte(block.data[i]);
			}
		}

		(void)hook_ymodem_tick(k_uptime_get_32());
		hook_ota_keepalive_c();
		hook_ota_session_timeout_check();

		if ((hook_ota_rx_drops != 0U) && ((hook_ota_rx_drops % 32U) == 0U)) {
			LOG_WRN("OTA receive queue overflow (%u blocks dropped)",
				hook_ota_rx_drops);
		}

		k_msleep(HOOK_OTA_THREAD_PERIOD_MS);
	}
}

K_THREAD_STACK_DEFINE(hook_ota_stack, 4096);
static struct k_thread hook_ota_thread_obj;

void hook_ota_start(void)
{
	k_thread_create(&hook_ota_http_thread_obj, hook_ota_http_stack,
			K_THREAD_STACK_SIZEOF(hook_ota_http_stack),
			hook_ota_http_thread, NULL, NULL, NULL,
			OSAL_PRIO_BELOW_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_ota_http_thread_obj, "OtaHttp");

	k_thread_create(&hook_ota_thread_obj, hook_ota_stack,
			K_THREAD_STACK_SIZEOF(hook_ota_stack),
			hook_ota_thread, NULL, NULL, NULL,
			OSAL_PRIO_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_ota_thread_obj, "OtaTask");
}
