/*
 * hook_4g.c - 4G / cloud channel handling.
 *
 * The usr.cn module delivers everything it receives from the MQTT broker on
 * one UART with a "<channel>," prefix:
 *
 *   "1," + craner#AT...   device AT command, reply with a "1," prefix
 *   "1," + usr.cn#AT...   command for the module itself (passthrough)
 *   "2," ...              reserved, ignored
 *   "3,"/"4," ...         cloud payload -> 0xA5 protocol parser
 *   "usr.cn#..."          echo of a modem command line
 *   anything else         cloud payload -> 0xA5 protocol parser
 *
 * There is deliberately no periodic traffic towards the module any more.
 * The old firmware sent "usr.cn#AT+Z" once per hour as a link keep-alive; on
 * this product the module restarts every time it receives it (observed on the
 * bench), so the controller no longer sends it. Keeping the MQTT socket alive
 * is left to the module/cloud and is tracked as a follow-up item.
 *
 * The UART receive callback runs in interrupt/timer context, so it only
 * queues the frame; all parsing and replying happens in the 4G thread.
 */
#include "hook_4g.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "hook_at.h"
#include "hook_cloud_proto.h"
#include "hook_health.h"
#include "hook_ota.h"
#include "offline_log_service.h"
#include "osal_zephyr.h"
#include "system_def.h"
#include "uart_manage.h"
#include "uart_manage_port.h"

LOG_MODULE_REGISTER(hook_4g, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * Frames delivered by uart_manage are published by the UART idle timer
 * (interrupt context), so they are appended to a single producer/single
 * consumer byte ring and their lengths are queued separately. Compared to
 * queueing whole frames this removes the (small) frame queue limit and the
 * copy-while-the-ISR-overwrites race, both of which could silently drop the
 * beginning of a long modem answer.
 */
#define HOOK_4G_RX_RING_SIZE    8192U
#define HOOK_4G_RX_BLOCK_SIZE   2050U
#define HOOK_4G_RX_LENQ_DEPTH   32U
/* Console echo/hex buffers: one shell_fprintf() call per buffer (the shell
 * erases the current line on every call, so keep the number of calls small).
 */
#define HOOK_4G_ECHO_BUF_SIZE   1024U

struct hook_4g_rx_block {
	uint16_t len;
	uint8_t data[HOOK_4G_RX_BLOCK_SIZE];
};

static uint8_t hook_4g_rx_ring_storage[HOOK_4G_RX_RING_SIZE];
static lwrb_t hook_4g_rx_ring;
static bool hook_4g_rx_ring_ready;

K_MSGQ_DEFINE(hook_4g_frame_len_queue, sizeof(uint16_t), HOOK_4G_RX_LENQ_DEPTH, 2);

K_MUTEX_DEFINE(hook_4g_tx_mutex);

static volatile uint32_t hook_4g_rx_frames_isr;
static volatile uint32_t hook_4g_rx_bytes_isr;
static volatile uint32_t hook_4g_rx_drops_isr;
static bool hook_4g_hex_echo_enabled;

void hook_4g_enable_hex_echo(bool enable)
{
	if (enable) {
		/* Counters describe the frames received from now on, so the dump
		 * of one command is unambiguous.
		 */
		hook_4g_rx_frames_isr = 0U;
		hook_4g_rx_bytes_isr = 0U;
		hook_4g_rx_drops_isr = 0U;
	}

	hook_4g_hex_echo_enabled = enable;
}

static void hook_4g_hex_dump(const uint8_t *buf, uint16_t len)
{
	static char text[HOOK_4G_ECHO_BUF_SIZE];
	uint16_t used;
	int written;

	/* Printed on the console (not through the log) so that the log rate
	 * limiter can never hide frames while debugging.
	 */
	written = snprintf(text, sizeof(text),
			   "\r\n[4g rx %u B frames=%u bytes=%u drops=%u]\r\n", len,
			   hook_4g_rx_frames_isr, hook_4g_rx_bytes_isr,
			   hook_4g_rx_drops_isr);
	if ((written > 0) && ((size_t)written < sizeof(text))) {
		(void)hook_console_send((const uint8_t *)text, (uint16_t)written);
	}

	used = 0U;
	for (uint16_t off = 0U; off < len; off = (uint16_t)(off + 16U)) {
		uint16_t row = (uint16_t)(len - off);
		int row_written;

		if (row > 16U) {
			row = 16U;
		}

		if ((sizeof(text) - used) < (3U * 16U + 16U)) {
			(void)hook_console_send((const uint8_t *)text, used);
			used = 0U;
		}

		row_written = snprintf(&text[used], sizeof(text) - used, "%04X:", off);
		if (row_written <= 0) {
			break;
		}
		used = (uint16_t)(used + row_written);

		for (uint16_t i = 0U; i < row; i++) {
			row_written = snprintf(&text[used], sizeof(text) - used, " %02X",
					       buf[off + i]);
			if (row_written <= 0) {
				break;
			}
			used = (uint16_t)(used + row_written);
		}

		text[used++] = '\r';
		text[used++] = '\n';
	}

	if (used > 0U) {
		(void)hook_console_send((const uint8_t *)text, used);
	}
}

int hook_4g_send(const uint8_t *data, uint16_t len)
{
	int ret;

	if ((data == NULL) || (len == 0U)) {
		return -1;
	}

	(void)k_mutex_lock(&hook_4g_tx_mutex, K_FOREVER);
	ret = (int)uart_manage_send_by_name("4g", data, len);
	k_mutex_unlock(&hook_4g_tx_mutex);

	return ret;
}

int hook_4g_send_ota(const uint8_t *data, uint16_t len)
{
	uint8_t frame[2U + 64U];
	int ret;

	if ((data == NULL) || (len == 0U) || (len > (sizeof(frame) - 2U))) {
		return -1;
	}

	/* Prefix and payload must leave the MCU in one usr.cn frame. */
	frame[0] = '2';
	frame[1] = ',';
	memcpy(&frame[2], data, len);

	(void)k_mutex_lock(&hook_4g_tx_mutex, K_FOREVER);
	ret = (int)uart_manage_send_by_name("4g", frame, (uint16_t)(len + 2U));
	k_mutex_unlock(&hook_4g_tx_mutex);

	return ret;
}

int32_t hook_4g_recv_callback(uint8_t *buf, uint16_t len)
{
	uint16_t frame_len = len;

	if ((buf == NULL) || (len == 0U) || !hook_4g_rx_ring_ready) {
		return -1;
	}

	/* Interrupt context: only the ring and the length queue are touched. */
	if (k_msgq_num_free_get(&hook_4g_frame_len_queue) == 0U) {
		hook_4g_rx_drops_isr++;
		return -1;
	}

	if (lwrb_get_free(&hook_4g_rx_ring) < frame_len) {
		hook_4g_rx_drops_isr++;
		return -1;
	}

	(void)lwrb_write(&hook_4g_rx_ring, buf, frame_len);

	if (k_msgq_put(&hook_4g_frame_len_queue, &frame_len, K_NO_WAIT) != 0) {
		hook_4g_rx_drops_isr++;
		return -1;
	}

	hook_4g_rx_frames_isr++;
	hook_4g_rx_bytes_isr += frame_len;

	return 0;
}

static int32_t hook_4g_mqtt_reply(const uint8_t *buf, uint16_t len)
{
	static const uint8_t prefix[] = "1,";
	uint8_t frame[2U + 512U];
	uint16_t offset = 0U;

	if ((buf == NULL) || (len == 0U)) {
		return -1;
	}

	/* Long answers (configuration dumps) are published in chunks. */
	while (offset < len) {
		uint16_t chunk = (uint16_t)(len - offset);

		if (chunk > (sizeof(frame) - 2U)) {
			chunk = (uint16_t)(sizeof(frame) - 2U);
		}

		memcpy(frame, prefix, 2U);
		memcpy(&frame[2], &buf[offset], chunk);
		(void)hook_4g_send(frame, (uint16_t)(chunk + 2U));

		offset = (uint16_t)(offset + chunk);
	}

	return 0;
}

/*
 * Text (printable ASCII, CR/LF/TAB allowed) carries no 0xA5 cloud frame, so
 * it is treated as a usr.cn module answer rather than as binary protocol.
 */
static bool hook_4g_is_printable_text(const uint8_t *buf, uint16_t len)
{
	for (uint16_t i = 0U; i < len; i++) {
		uint8_t c = buf[i];

		if ((c == '\r') || (c == '\n') || (c == '\t')) {
			continue;
		}
		if ((c < 0x20U) || (c > 0x7EU)) {
			return false;
		}
	}

	return (len > 0U);
}

/* Percentage of printable characters (used to spot slightly corrupted text). */
static uint8_t hook_4g_text_ratio(const uint8_t *buf, uint16_t len)
{
	uint16_t printable = 0U;

	if (len == 0U) {
		return 0U;
	}

	for (uint16_t i = 0U; i < len; i++) {
		uint8_t c = buf[i];

		if (((c >= 0x20U) && (c <= 0x7EU)) || (c == '\r') || (c == '\n') ||
		    (c == '\t')) {
			printable++;
		}
	}

	return (uint8_t)((printable * 100U) / len);
}

/*
 * Print the whole frame on a fresh console line, replacing non printable
 * bytes with '.'.
 *
 * The frame is printed with one shell_fprintf() call per buffer: every call
 * outside a command context first erases the current line and then restores
 * the prompt, so splitting a modem answer over several calls would erase the
 * beginning of the lines it just printed. Only answers larger than the echo
 * buffer are split (rare: they are configuration dumps of a few hundred
 * bytes).
 */
/*
 * Frame model (see docs/4G数据帧解析.md, based on the WH-LTE-7S1 manual):
 *
 *  1) "<num>,<payload>"  num = 0..10 = subscribe topic number (distribution
 *     mode). 1 = system/remote-debug topic (craner#AT / usr.cn#AT commands),
 *     2 = OTA topic, 3..10 (and 0 = temporary topic) = host management data.
 *  2) "usr.cn#..."       module's own AT answer/echo -> shell + topic 1.
 *  3) anything else      legacy binary payload -> host protocol parser.
 *
 * The module may split one answer over several UART idle-delimited chunks,
 * so a command answer is reassembled until its CRLF + OK/ERROR terminator,
 * and a payload that lost its "<num>," prefix keeps feeding the consumer it
 * was routed to.
 */
#define HOOK_4G_AT_BUF_SIZE            1024U
#define HOOK_4G_AT_RESPONSE_TIMEOUT_MS 3000U
#define HOOK_4G_ROUTE_STICKY_MS        1000U

enum hook_4g_route {
	HOOK_4G_ROUTE_NONE = 0,
	HOOK_4G_ROUTE_OTA,
	HOOK_4G_ROUTE_LEGACY,
};

static uint8_t hook_4g_at_buf[HOOK_4G_AT_BUF_SIZE];
static uint16_t hook_4g_at_len;
static bool hook_4g_at_active;
static bool hook_4g_at_overflow;
static uint32_t hook_4g_at_deadline_ms;
static enum hook_4g_route hook_4g_last_route;
static uint32_t hook_4g_last_route_ms;

static void hook_4g_echo_sanitized(const uint8_t *buf, uint16_t len)
{
	static const uint8_t crlf[] = "\r\n";
	static char text[HOOK_4G_ECHO_BUF_SIZE];
	uint16_t offset = 0U;

	(void)hook_console_send(crlf, 2U);

	while (offset < len) {
		uint16_t chunk = (uint16_t)(len - offset);

		if (chunk > (sizeof(text) - 1U)) {
			chunk = (uint16_t)(sizeof(text) - 1U);
		}

		for (uint16_t i = 0U; i < chunk; i++) {
			uint8_t c = buf[offset + i];

			text[i] = (((c >= 0x20U) && (c <= 0x7EU)) || (c == '\r') ||
				   (c == '\n') || (c == '\t')) ? (char)c : '.';
		}
		text[chunk] = '\0';

		(void)hook_console_send((const uint8_t *)text, chunk);
		offset = (uint16_t)(offset + chunk);
	}
}

/*
 * Parse the "<num>," topic prefix of a distribution-mode frame.
 * Returns the prefix length (0 when this is not a topic frame).
 */
static uint8_t hook_4g_parse_topic_prefix(const uint8_t *buf, uint16_t len,
					  uint8_t *topic)
{
	uint16_t index = 0U;
	uint16_t value = 0U;

	while ((index < len) && (index < 2U) && (buf[index] >= '0') &&
	       (buf[index] <= '9')) {
		value = (uint16_t)((value * 10U) + (uint16_t)(buf[index] - '0'));
		index++;
	}

	if ((index == 0U) || (index >= len) || (buf[index] != ',') || (value > 10U)) {
		return 0U;
	}

	*topic = (uint8_t)value;

	return (uint8_t)(index + 1U);
}

/* Start collecting a module AT answer (called when a usr.cn command is sent). */
void hook_4g_at_exchange_start(void)
{
	hook_4g_at_len = 0U;
	hook_4g_at_overflow = false;
	hook_4g_at_active = true;
	hook_4g_at_deadline_ms = k_uptime_get_32() + HOOK_4G_AT_RESPONSE_TIMEOUT_MS;
}

/* Deliver the collected AT answer (console + topic 1) and end the exchange. */
static void hook_4g_at_exchange_finish(void)
{
	if (!hook_4g_at_active) {
		return;
	}

	hook_4g_at_active = false;

	if (hook_4g_at_len == 0U) {
		return;
	}

	hook_4g_echo_sanitized(hook_4g_at_buf, hook_4g_at_len);
	(void)hook_4g_mqtt_reply(hook_4g_at_buf, hook_4g_at_len);

	if (hook_4g_at_overflow) {
		LOG_WRN("AT answer longer than %u B, tail dropped",
			(unsigned int)sizeof(hook_4g_at_buf));
	}
}

/* True when the buffer already contains the module's end-of-answer marker. */
static bool hook_4g_at_has_terminator(const uint8_t *buf, uint16_t len)
{
	static const char ok[] = "OK\r\n";
	static const char err[] = "ERROR\r\n";

	if (len < sizeof(ok) - 1U) {
		return false;
	}

	/* Only the tail can hold the terminator of the current answer. */
	uint16_t start = (len > 16U) ? (uint16_t)(len - 16U) : 0U;

	for (uint16_t i = start; i < len; i++) {
		if ((uint16_t)(len - i) >= sizeof(ok) - 1U) {
			if (memcmp(&buf[i], ok, sizeof(ok) - 1U) == 0) {
				return true;
			}
		}
		if ((uint16_t)(len - i) >= sizeof(err) - 1U) {
			if (memcmp(&buf[i], err, sizeof(err) - 1U) == 0) {
				return true;
			}
		}
	}

	return false;
}

/* Append one received chunk to the pending AT answer. */
static bool hook_4g_at_exchange_feed(const uint8_t *buf, uint16_t len)
{
	uint16_t space = (uint16_t)(sizeof(hook_4g_at_buf) - hook_4g_at_len);
	uint16_t copy = (len < space) ? len : space;

	if (copy > 0U) {
		memcpy(&hook_4g_at_buf[hook_4g_at_len], buf, copy);
		hook_4g_at_len = (uint16_t)(hook_4g_at_len + copy);
	}
	if (copy < len) {
		hook_4g_at_overflow = true;
	}

	if (hook_4g_at_has_terminator(hook_4g_at_buf, hook_4g_at_len)) {
		hook_4g_at_exchange_finish();
		return true;
	}

	return false;
}

/* Timeout guard, called from the 4G thread. */
static void hook_4g_at_exchange_timeout_check(void)
{
	if (!hook_4g_at_active) {
		return;
	}

	if ((int32_t)(k_uptime_get_32() - hook_4g_at_deadline_ms) < 0) {
		return;
	}

	LOG_WRN("AT answer incomplete after %u ms, delivering %u B",
		(unsigned int)HOOK_4G_AT_RESPONSE_TIMEOUT_MS, hook_4g_at_len);
	hook_4g_at_exchange_finish();
}

/* Route one distribution-mode frame according to its topic number. */
static void hook_4g_dispatch_topic(uint8_t topic, const uint8_t *payload,
				   uint16_t payload_len, const uint8_t *frame,
				   uint16_t frame_len)
{
	switch (topic) {
	case 1: { /* system topic: remote debug port, same commands as the shell */
		int32_t ret = hook_at_craner_handler(payload, payload_len,
						     hook_4g_mqtt_reply);

		if (ret != HOOK_AT_PREFIX_NOT_MATCH) {
			return;
		}

		ret = hook_at_usr_handler(payload, payload_len);
		if (ret != HOOK_AT_PREFIX_NOT_MATCH) {
			return;
		}

		LOG_WRN_RATELIMIT("unmatched topic 1 frame from host");
		if (hook_4g_text_ratio(frame, frame_len) >= 80U) {
			hook_4g_echo_sanitized(frame, frame_len);
		}
		break;
	}

	case 2: /* OTA topic: Ymodem image data / progress */
		if (hook_ota_is_active()) {
			hook_ota_feed(payload, payload_len);
			hook_4g_last_route = HOOK_4G_ROUTE_OTA;
			hook_4g_last_route_ms = k_uptime_get_32();
		} else if (hook_4g_text_ratio(frame, frame_len) >= 80U) {
			LOG_WRN_RATELIMIT("OTA data ignored: no OTA session active");
			hook_4g_echo_sanitized(frame, frame_len);
		}
		break;

	default: /* 0 = temporary topic, 3..10 = host management topics */
		if (hook_4g_text_ratio(frame, frame_len) >= 80U) {
			hook_4g_echo_sanitized(frame, frame_len);
		}
		hook_cloud_proto_feed(payload, payload_len);
		hook_4g_last_route = HOOK_4G_ROUTE_LEGACY;
		hook_4g_last_route_ms = k_uptime_get_32();
		break;
	}
}

static void hook_4g_process_block(const struct hook_4g_rx_block *block)
{
	const uint8_t *buf = block->data;
	uint16_t len = block->len;
	uint8_t topic = 0U;
	uint8_t prefix_len;

	if (hook_4g_hex_echo_enabled) {
		hook_4g_hex_dump(buf, len);
	}

	/* 1) MQTT distribution frame: "<num>,<payload>" */
	prefix_len = hook_4g_parse_topic_prefix(buf, len, &topic);
	if (prefix_len > 0U) {
		hook_4g_dispatch_topic(topic, &buf[prefix_len],
				       (uint16_t)(len - prefix_len), buf, len);
		return;
	}

	/* 2) A module AT answer is being reassembled: feed it until OK/ERROR. */
	if (hook_4g_at_active) {
		(void)hook_4g_at_exchange_feed(buf, len);
		return;
	}

	/*
	 * 3) Continuation of a payload whose "<num>," prefix was in an earlier
	 * chunk (the UART idle timer can split one module message).
	 */
	if ((hook_4g_last_route != HOOK_4G_ROUTE_NONE) &&
	    ((uint32_t)(k_uptime_get_32() - hook_4g_last_route_ms) <
	     HOOK_4G_ROUTE_STICKY_MS)) {
		if (hook_4g_last_route == HOOK_4G_ROUTE_OTA) {
			if (hook_ota_is_active()) {
				hook_ota_feed(buf, len);
			}
		} else {
			hook_cloud_proto_feed(buf, len);
		}
		return;
	}
	hook_4g_last_route = HOOK_4G_ROUTE_NONE;

	/* 4) Module's own text (usr.cn# echo or plain AT answer). */
	if ((len >= 7U) && (memcmp(buf, "usr.cn#", 7U) == 0)) {
		(void)hook_console_send(&buf[7], (uint16_t)(len - 7U));
		(void)hook_4g_mqtt_reply(&buf[7], (uint16_t)(len - 7U));
		return;
	}

	if (hook_4g_is_printable_text(buf, len)) {
		hook_4g_echo_sanitized(buf, len);
		(void)hook_4g_mqtt_reply(buf, len);
		return;
	}

	/* Mostly printable (a stray control byte): still show it. */
	if (hook_4g_text_ratio(buf, len) >= 80U) {
		hook_4g_echo_sanitized(buf, len);
	}

	/* 5) Legacy binary payload without any prefix. Never drop it silently:
	 * the hex head makes such frames visible in the log.
	 */
	LOG_INF_RATELIMIT("4g rx %u B (not text): %02X %02X %02X %02X %02X %02X %02X %02X",
			  len, (len > 0U) ? buf[0] : 0U, (len > 1U) ? buf[1] : 0U,
			  (len > 2U) ? buf[2] : 0U, (len > 3U) ? buf[3] : 0U,
			  (len > 4U) ? buf[4] : 0U, (len > 5U) ? buf[5] : 0U,
			  (len > 6U) ? buf[6] : 0U, (len > 7U) ? buf[7] : 0U);

	hook_cloud_proto_feed(buf, len);
}

static void hook_4g_thread(void *a, void *b, void *c)
{
	/* 2 KiB scratch outside the thread stack (a frame may hold a whole
	 * Ymodem packet).
	 */
	static struct hook_4g_rx_block rx_block;
	uint32_t last_err_check_ms = 0U;
	const struct device *modem_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));

	(void)a;
	(void)b;
	(void)c;

	hook_cloud_proto_init();

	for (;;) {
		uint16_t frame_len = 0U;

		hook_health_task_alive(HOOK_TASK_ALIVE_4G);

		if (k_msgq_get(&hook_4g_frame_len_queue, &frame_len, K_MSEC(50)) == 0) {
			uint16_t take = (frame_len > sizeof(rx_block.data)) ?
						(uint16_t)sizeof(rx_block.data) :
						frame_len;

			(void)lwrb_read(&hook_4g_rx_ring, rx_block.data, take);
			if (frame_len > take) {
				/* Cannot happen: frames are bounded by the uart_manage
				 * receive buffer (2048 B).
				 */
				hook_4g_rx_drops_isr++;
			}
			rx_block.len = take;

			hook_4g_process_block(&rx_block);
		}

		hook_4g_at_exchange_timeout_check();

		if (hook_4g_rx_drops_isr != 0U) {
			static uint32_t hook_4g_drops_logged;

			LOG_WRN_RATELIMIT("4G receive overflow: %u frame(s) dropped "
					  "(ring/len queue full)",
					  hook_4g_rx_drops_isr);

			/* Keep the drop history on the SD card as well (once per new
			 * total, not per loop pass).
			 */
			if (hook_4g_rx_drops_isr > hook_4g_drops_logged) {
				hook_4g_drops_logged = hook_4g_rx_drops_isr;
				(void)offline_log_event(
					OFFLINE_LOG_CAT_NETWORK,
					OFFLINE_LOG_LEVEL_WRN,
					"4G receive overflow: %u frame(s) dropped "
					"(ring/len queue full)",
					hook_4g_rx_drops_isr);
			}
		}

		/* Receiver errors (overrun/framing) silently lose bytes, which
		 * is another way a long modem answer can be cut short.
		 */
		if ((k_uptime_get_32() - last_err_check_ms) >= 500U) {
			int err = uart_err_check(modem_uart);

			if (err > 0) {
				LOG_WRN_RATELIMIT("4g uart error flags 0x%02X", err);
			}
			last_err_check_ms = k_uptime_get_32();
		}
	}
}

K_THREAD_STACK_DEFINE(hook_4g_stack, 4096);
static struct k_thread hook_4g_thread_obj;

void hook_4g_init(void)
{
	(void)lwrb_init(&hook_4g_rx_ring, hook_4g_rx_ring_storage,
			(uint32_t)sizeof(hook_4g_rx_ring_storage));
	k_msgq_purge(&hook_4g_frame_len_queue);
	hook_4g_rx_frames_isr = 0U;
	hook_4g_rx_bytes_isr = 0U;
	hook_4g_rx_drops_isr = 0U;
	hook_4g_rx_ring_ready = true;

	hook_cloud_proto_init();
}

void hook_4g_start(void)
{
	k_thread_create(&hook_4g_thread_obj, hook_4g_stack,
			K_THREAD_STACK_SIZEOF(hook_4g_stack),
			hook_4g_thread, NULL, NULL, NULL,
			OSAL_PRIO_NORMAL1, 0, K_NO_WAIT);
	k_thread_name_set(&hook_4g_thread_obj, "App4GTask");
}
