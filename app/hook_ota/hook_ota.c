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

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "hook_health.h"
#include "hook_4g.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "hook_ymodem.h"
#include "osal_zephyr.h"
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
	hook_ota_report("DONE");

	LOG_WRN("OTA staged: rebooting in %u ms to let MCUboot install slot1",
		(unsigned int)CONFIG_HOOK_OTA_REBOOT_DELAY_MS);
}

static void hook_ota_ymodem_abort(hook_ymodem_status_t reason, void *user)
{
	ARG_UNUSED(user);

	hook_ota_flash_active = false;
	hook_ota_set_state(HOOK_OTA_ERROR);
	hook_ota_report((reason == HOOK_YMODEM_ERR_ABORTED) ? "ERROR,20" :
							      "ERROR,21");
	hook_health_ota_gate(false);

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

void hook_ota_init(void)
{
	hook_ymodem_init(&hook_ota_ymodem_config);
	hook_ota_state = HOOK_OTA_IDLE;
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
	if (!hook_ota_is_active()) {
		LOG_INF("OTA cancel: no session active");
		hook_ymodem_reset();
		hook_ota_set_state(HOOK_OTA_IDLE);
		return;
	}

	LOG_WRN("OTA cancel requested");

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

	if ((k_uptime_get_32() - hook_ota_last_activity_ms) <
	    CONFIG_HOOK_OTA_SESSION_TIMEOUT_MS) {
		return;
	}

	LOG_ERR("OTA session timeout (%u ms without data), aborting",
		(unsigned int)CONFIG_HOOK_OTA_SESSION_TIMEOUT_MS);
	hook_ota_report("ERROR,22");
	hook_ota_cancel();
}

static void hook_ota_reboot_check(void)
{
	if (hook_ota_state != HOOK_OTA_STAGED) {
		return;
	}

	/* Give the module time to push "DONE" to MQTT before resetting. */
	if (k_uptime_get() <
	    (int64_t)(hook_ota_last_activity_ms + CONFIG_HOOK_OTA_REBOOT_DELAY_MS)) {
		return;
	}

	sys_reboot(SYS_REBOOT_COLD);
}

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
		hook_ota_reboot_check();

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
	k_thread_create(&hook_ota_thread_obj, hook_ota_stack,
			K_THREAD_STACK_SIZEOF(hook_ota_stack),
			hook_ota_thread, NULL, NULL, NULL,
			OSAL_PRIO_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_ota_thread_obj, "OtaTask");
}
