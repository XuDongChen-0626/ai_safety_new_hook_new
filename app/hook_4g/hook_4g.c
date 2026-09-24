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

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "hook_at.h"
#include "board.h"
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
/*
 * Big enough to absorb the stalls of the HTTP OTA path: this thread writes
 * every received block into slot1, and an internal-flash sector erase blocks
 * it for seconds while the module keeps streaming at the serial line rate
 * (~11 KB/s, so a 3 s erase already costs ~35 KiB).
 */
#define HOOK_4G_RX_RING_SIZE    131072U
#define HOOK_4G_RX_BLOCK_SIZE   2050U
#define HOOK_4G_RX_LENQ_DEPTH   64U
/* Supply off-time of the module hard reset: long enough for its rails to
 * discharge so the module really restarts from scratch.
 */
#define HOOK_4G_MODEM_POWER_OFF_MS 500U
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
/* True while the module is in HTTPD mode (file download in progress). */
static volatile bool hook_4g_http_mode;

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

/* Raw transmit: used by the AT channel and by the HTTP trigger. */
static int hook_4g_send_raw(const uint8_t *data, uint16_t len)
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

int hook_4g_send(const uint8_t *data, uint16_t len)
{
	/*
	 * In HTTPD mode anything sent to the module is appended to the HTTP
	 * request being built, so business data must not go out at all.
	 */
	if (hook_4g_http_mode) {
		return -EBUSY;
	}

	return hook_4g_send_raw(data, len);
}

int hook_4g_send_ota(const uint8_t *data, uint16_t len)
{
	uint8_t frame[2U + 64U];
	int ret;

	if ((data == NULL) || (len == 0U) || (len > (sizeof(frame) - 2U))) {
		return -1;
	}

	if (hook_4g_http_mode) {
		return -EBUSY;
	}

	/* Prefix and payload must leave the MCU in one usr.cn frame. */
	frame[0] = '2';
	frame[1] = ',';
	memcpy(&frame[2], data, len);

	ret = hook_4g_send_raw(frame, (uint16_t)(len + 2U));

	return ret;
}

int hook_4g_http_trigger(const char *host, uint16_t port)
{
	char line[192];
	int written;

	if ((host == NULL) || (host[0] == '\0')) {
		return -EINVAL;
	}

	/*
	 * The module builds "GET <AT+HTPURL> " itself and appends whatever the
	 * serial line delivers, so this is the tail that turns it into a valid
	 * HTTP/1.1 request (version + Host + terminating blank line).
	 *
	 * The leading space covers the case where the module does NOT insert its
	 * own separator (observed: "/s/xigzYCNUFNHTTP/1.1" -> 404): HTTP allows
	 * several spaces between the request line elements, so a doubled space is
	 * harmless, while a missing one produces a wrong URL.
	 */
	if (port == 80U) {
		written = snprintf(line, sizeof(line),
				   " HTTP/1.1\r\nHost:%s\r\n\r\n", host);
	} else {
		written = snprintf(line, sizeof(line),
				   " HTTP/1.1\r\nHost:%s:%u\r\n\r\n", host,
				   (unsigned int)port);
	}
	if ((written <= 0) || ((size_t)written >= sizeof(line))) {
		return -EINVAL;
	}

	LOG_INF("HTTP OTA: triggering GET, %d byte(s) of request tail", written);

	return hook_4g_send_raw((const uint8_t *)line, (uint16_t)written);
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

	if (hook_4g_http_mode) {
		/* Muted: the module would append this to the HTTP request. */
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
/* Longest "usr.cn#" + AT command (a full MQTTPUBTP line fits twice over). */
#define HOOK_4G_AT_CMD_MAX_LEN         192U
/* How long hook_4g_at_cmd() waits for the AT channel before giving up. */
#define HOOK_4G_AT_CMD_LOCK_MS         3500U
/* Extra grace for the 4G thread to close an exchange we timed out on. */
#define HOOK_4G_AT_CMD_DRAIN_MS        500U

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
/* Requested wait of the exchange being collected (0 = none). */
static uint32_t hook_4g_at_timeout_ms;
static enum hook_4g_route hook_4g_last_route;
static uint32_t hook_4g_last_route_ms;

/*
 * Synchronous exchange context (hook_4g_at_cmd).
 *
 * It is deliberately a single static object: only one synchronous exchange
 * can run at a time (arbitrated by hook_4g_at_mutex) and the 4G thread may
 * still be finishing the answer when the caller times out. Keeping the
 * context out of the caller's stack makes that race harmless - the late
 * answer is simply written into a context nobody looks at any more.
 */
struct hook_4g_at_wait {
	char resp[HOOK_4G_AT_BUF_SIZE];
	uint16_t len;
	int result;
	bool pending;
};

static struct hook_4g_at_wait hook_4g_at_wait;
/* Signalled by the 4G thread when a synchronous exchange completes. */
K_SEM_DEFINE(hook_4g_at_wait_done, 0, 1);
/* Serialises synchronous exchanges; held for the whole exchange. */
static K_MUTEX_DEFINE(hook_4g_at_mutex);
/* Set when the module prints its boot banner, cleared before a module restart. */
static volatile bool hook_4g_modem_boot_seen_flag;

bool hook_4g_modem_boot_seen(void)
{
	return hook_4g_modem_boot_seen_flag;
}

void hook_4g_modem_boot_reset(void)
{
	hook_4g_modem_boot_seen_flag = false;
}

/*
 * Hard reset of the module: cycle its 3.3 V rail (PA15 = POWER_4G_3V3).
 *
 * AT+S / AT+CLEAR only make the module restart itself, which is the right
 * mechanism for "apply the configuration I just saved" but no guarantee at all
 * against a module whose AT interpreter or MQTT client is stuck - and the
 * module answers "OK" before it restarts, so a lost answer cannot be told
 * apart from a lost module.
 *
 * Cutting the rail is the same thing a controller reset does to the module on
 * this board (the banner comes back ~7 s after every MCU reset), so it is the
 * reset with a guarantee. The stack keeps its state, so a staged image or the
 * OTA progress is not affected.
 */
void hook_4g_modem_hard_reset(void)
{
	unsigned int key;

	(void)gpio_pin_set_dt(&modem_power_gpio, 0);
	k_msleep(HOOK_4G_MODEM_POWER_OFF_MS);
	(void)gpio_pin_set_dt(&modem_power_gpio, 1);

	/*
	 * Drop whatever the module had already put on the wire: those frames
	 * belong to the session that just died. Locked because the UART
	 * callback fills the ring from interrupt context.
	 */
	key = irq_lock();
	if (hook_4g_rx_ring_ready) {
		lwrb_reset(&hook_4g_rx_ring);
	}
	k_msgq_purge(&hook_4g_frame_len_queue);
	irq_unlock(key);

	hook_4g_modem_boot_reset();

	LOG_WRN("4G module hard reset: 3V3 rail off %u ms, waiting for the banner",
		(unsigned int)HOOK_4G_MODEM_POWER_OFF_MS);
}

void hook_4g_set_http_mode(bool on)
{
	if (hook_4g_http_mode == on) {
		return;
	}

	hook_4g_http_mode = on;

	LOG_INF("4G channel: %s", on ?
			"HTTPD mode (business TX muted, RX -> HTTP OTA)" :
			"normal mode");
}

bool hook_4g_is_http_mode(void)
{
	return hook_4g_http_mode;
}

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

/* Substring search over a received block (blocks are not NUL terminated). */
static bool hook_4g_contains(const uint8_t *buf, uint16_t len,
			     const char *needle)
{
	size_t needle_len = strlen(needle);

	if ((needle_len == 0U) || ((size_t)len < needle_len)) {
		return false;
	}

	for (uint16_t i = 0U; (size_t)(len - i) >= needle_len; i++) {
		if (memcmp(&buf[i], needle, needle_len) == 0) {
			return true;
		}
	}

	return false;
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

/*
 * True when the answer holds "OK"/"ERROR" at @p index followed by a line end.
 *
 * Both CRLF and a bare LF are accepted: module firmware revisions differ, and
 * a mismatch here made every synchronous exchange time out even though the
 * answer had arrived.
 */
static bool hook_4g_at_token_at(const uint8_t *buf, uint16_t len,
				uint16_t index, const char *token)
{
	size_t token_len = strlen(token);

	if ((size_t)(len - index) < (token_len + 1U)) {
		return false;
	}

	if (memcmp(&buf[index], token, token_len) != 0) {
		return false;
	}

	return (buf[index + token_len] == '\n') ||
	       (buf[index + token_len] == '\r');
}

/*
 * Format up to 24 bytes as "AA BB CC " for a log line.
 *
 * A frame that cannot be parsed is a dead end without its bytes - the two
 * cases that cost real debugging time were an AT answer of 24 apparently
 * meaningless bytes (it was "+CME ERROR:50") and topic 1 commands that arrive
 * without a known prefix.
 */
static void hook_4g_hex_text(char *out, size_t out_len, const uint8_t *buf,
			     uint16_t len)
{
	uint16_t shown = MIN(len, 24U);
	size_t used = 0U;

	for (uint16_t i = 0U; (i < shown) && ((used + 4U) <= out_len); i++) {
		int n = snprintf(&out[used], out_len - used, "%02X ", buf[i]);

		if (n <= 0) {
			break;
		}

		used += (size_t)n;
	}

	out[MIN(used, out_len - 1U)] = '\0';
}

/*
 * True when the answer carries a terminal error line of its own:
 * "+CME ERROR:<code>" (module/network error) or "+CMS ERROR:<code>". Per
 * 27.007 such a line REPLACES the OK/ERROR terminator, so the answer is
 * complete - and it is the module telling us that something is wrong. Without
 * this the collector waited for its full deadline on every such answer ("AT
 * answer incomplete after 3000 ms"), which is what made AT queries look slow.
 */
static bool hook_4g_at_final_error_line(const uint8_t *buf, uint16_t len)
{
	static const char *const names[] = { "+CME ERROR", "+CMS ERROR" };

	for (size_t n = 0U; n < ARRAY_SIZE(names); n++) {
		size_t name_len = strlen(names[n]);

		if ((size_t)len < name_len) {
			continue;
		}

		for (uint16_t i = 0U; ((size_t)i + name_len) <= (size_t)len; i++) {
			if (memcmp(&buf[i], names[n], name_len) == 0) {
				return true;
			}
		}
	}

	return false;
}

/* Classify a complete AT answer by its terminator. */
static int hook_4g_at_tail_result(const uint8_t *buf, uint16_t len)
{
	uint16_t start = (len > 16U) ? (uint16_t)(len - 16U) : 0U;

	if (hook_4g_at_final_error_line(buf, len)) {
		return -EIO;
	}

	for (uint16_t i = start; i < len; i++) {
		if (hook_4g_at_token_at(buf, len, i, "ERROR")) {
			return -EIO;
		}

		if (hook_4g_at_token_at(buf, len, i, "OK")) {
			return 0;
		}
	}

	return -EBADMSG;
}

/* Start collecting a module AT answer (called when a usr.cn command is sent). */
int hook_4g_at_exchange_start(void)
{
	if (hook_4g_at_wait.pending) {
		/* A synchronous exchange owns the AT channel. */
		return -EBUSY;
	}

	hook_4g_at_len = 0U;
	hook_4g_at_overflow = false;
	hook_4g_at_timeout_ms = HOOK_4G_AT_RESPONSE_TIMEOUT_MS;
	hook_4g_at_deadline_ms = k_uptime_get_32() + hook_4g_at_timeout_ms;
	/* Publish the deadline before the exchange becomes visible to the 4G
	 * thread, otherwise its timeout check may close the exchange with the
	 * previous deadline.
	 */
	hook_4g_at_active = true;

	return 0;
}

/* Deliver the collected AT answer (console + topic 1) and end the exchange. */
static void hook_4g_at_exchange_finish(void)
{
	if (!hook_4g_at_active) {
		return;
	}

	hook_4g_at_active = false;

	if (hook_4g_at_wait.pending) {
		/*
		 * A synchronous caller is waiting: hand the raw answer over
		 * without echoing it on the shell or mirroring it to MQTT -
		 * this exchange may carry the MQTT password.
		 */
		uint16_t copy = MIN(hook_4g_at_len,
				    (uint16_t)(sizeof(hook_4g_at_wait.resp) - 1U));

		memcpy(hook_4g_at_wait.resp, hook_4g_at_buf, copy);
		hook_4g_at_wait.resp[copy] = '\0';
		hook_4g_at_wait.len = copy;

		if (copy > 0U) {
			hook_4g_at_wait.result =
				hook_4g_at_tail_result(hook_4g_at_buf, copy);
			if (hook_4g_at_wait.result == -EBADMSG) {
				/*
				 * Show what the module actually said: not every
				 * answer is terminated by OK/ERROR (this build
				 * answers AT+CIP with a bare "+CIP:<address>"),
				 * and without the bytes the warning is a
				 * dead end.
				 */
				char hex[(3U * 24U) + 1U];

				hook_4g_hex_text(hex, sizeof(hex),
						 hook_4g_at_buf, copy);
				LOG_WRN("AT answer carries no OK/ERROR (%u B collected): %s",
					copy, hex);
			} else if ((hook_4g_at_wait.result == -EIO) &&
				   hook_4g_at_final_error_line(hook_4g_at_buf,
							       copy)) {
				/* The module reported a problem itself: say what
				 * it said, this is the useful part.
				 */
				char text[64];
				size_t used = 0U;

				for (uint16_t i = 0U;
				     (i < copy) && (used < (sizeof(text) - 1U));
				     i++) {
					uint8_t ch = hook_4g_at_buf[i];

					text[used++] =
						((ch >= 0x20U) && (ch < 0x7FU)) ?
							(char)ch : ' ';
				}
				text[used] = '\0';

				LOG_WRN("AT answer is a module error: %s", text);
			}
		} else {
			hook_4g_at_wait.result = -ETIMEDOUT;
		}

		k_sem_give(&hook_4g_at_wait_done);
		return;
	}

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
	uint16_t start;

	if (len < 3U) {
		return false;
	}

	/* Only the tail can hold the terminator of the current answer. */
	start = (len > 16U) ? (uint16_t)(len - 16U) : 0U;

	for (uint16_t i = start; i < len; i++) {
		if (hook_4g_at_token_at(buf, len, i, "OK") ||
		    hook_4g_at_token_at(buf, len, i, "ERROR")) {
			return true;
		}
	}

	return false;
}

/* Append one received chunk to the pending AT answer. */
static bool hook_4g_at_exchange_feed(const uint8_t *buf, uint16_t len)
{
	/*
	 * A module restart puts 0x00 bytes on the UART (framing errors). They
	 * are not part of an AT answer, which is always text: skipping them
	 * keeps a noise byte that slipped into the same block from breaking the
	 * OK/ERROR terminator detection.
	 */
	for (uint16_t i = 0U; i < len; i++) {
		if (buf[i] == 0x00U) {
			continue;
		}

		if (hook_4g_at_len < (uint16_t)sizeof(hook_4g_at_buf)) {
			hook_4g_at_buf[hook_4g_at_len++] = buf[i];
		} else {
			hook_4g_at_overflow = true;
		}
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

	if (hook_4g_at_wait.pending) {
		/* The synchronous caller reports its own timeout. */
		LOG_DBG("AT exchange timed out after %u ms (%u B collected)",
			(unsigned int)hook_4g_at_timeout_ms, hook_4g_at_len);
	} else {
		LOG_WRN("AT answer incomplete after %u ms, delivering %u B",
			(unsigned int)hook_4g_at_timeout_ms, hook_4g_at_len);
	}
	hook_4g_at_exchange_finish();
}

int hook_4g_at_cmd(const char *cmd, char *resp, size_t resp_len,
		   uint32_t timeout_ms)
{
	char line[HOOK_4G_AT_CMD_MAX_LEN];
	int written;
	int ret;

	if ((cmd == NULL) || (cmd[0] == '\0') || (timeout_ms == 0U)) {
		return -EINVAL;
	}

	if (k_mutex_lock(&hook_4g_at_mutex,
			 K_MSEC(HOOK_4G_AT_CMD_LOCK_MS)) != 0) {
		return -EBUSY;
	}

	/* Let a hand typed exchange (shell "hook usr") finish first. */
	for (uint32_t waited = 0U; hook_4g_at_active; waited += 50U) {
		if (waited >= HOOK_4G_AT_RESPONSE_TIMEOUT_MS) {
			k_mutex_unlock(&hook_4g_at_mutex);
			return -EBUSY;
		}
		k_msleep(50);
	}

	written = snprintf(line, sizeof(line), "usr.cn#%s\r\n", cmd);
	if ((written <= 0) || ((size_t)written >= sizeof(line))) {
		k_mutex_unlock(&hook_4g_at_mutex);
		return -EINVAL;
	}

	k_sem_reset(&hook_4g_at_wait_done);
	hook_4g_at_wait.len = 0U;
	hook_4g_at_wait.result = -ETIMEDOUT;
	hook_4g_at_wait.resp[0] = '\0';
	hook_4g_at_wait.pending = true;

	hook_4g_at_len = 0U;
	hook_4g_at_overflow = false;
	hook_4g_at_timeout_ms = timeout_ms;
	hook_4g_at_deadline_ms = k_uptime_get_32() + timeout_ms;
	hook_4g_at_active = true;

	{
		/* Raw: the AT channel stays usable in HTTPD mode as well. */
		int sent = (int)hook_4g_send_raw((const uint8_t *)line,
						 (uint16_t)strlen(line));

		/* uart_manage returns UART_MANAGE_OK (0) on success. */
		if (sent < 0) {
			hook_4g_at_active = false;
			hook_4g_at_wait.pending = false;
			k_mutex_unlock(&hook_4g_at_mutex);
			LOG_ERR("AT command '%s' could not be sent (%d)", cmd,
				sent);
			return -EIO;
		}
	}

	if (k_sem_take(&hook_4g_at_wait_done, K_MSEC(timeout_ms)) != 0) {
		/*
		 * The 4G thread closes the exchange on its own deadline; give it
		 * a moment so a late answer is not echoed to the shell / MQTT
		 * afterwards. Only when it stays silent (thread stalled) is the
		 * exchange dropped here.
		 */
		if (k_sem_take(&hook_4g_at_wait_done,
			       K_MSEC(HOOK_4G_AT_CMD_DRAIN_MS)) != 0) {
			hook_4g_at_active = false;
			hook_4g_at_wait.pending = false;
			k_mutex_unlock(&hook_4g_at_mutex);
			return -ETIMEDOUT;
		}

		hook_4g_at_wait.pending = false;
		k_mutex_unlock(&hook_4g_at_mutex);
		return -ETIMEDOUT;
	}

	hook_4g_at_wait.pending = false;
	ret = hook_4g_at_wait.result;

	if ((resp != NULL) && (resp_len > 0U)) {
		size_t copy = MIN((size_t)hook_4g_at_wait.len, resp_len - 1U);

		memcpy(resp, hook_4g_at_wait.resp, copy);
		resp[copy] = '\0';
	}

	k_mutex_unlock(&hook_4g_at_mutex);

	return ret;
}

bool hook_4g_at_cmd_is_active(void)
{
	return hook_4g_at_wait.pending;
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

		{
			char hex[(3U * 24U) + 1U];

			hook_4g_hex_text(hex, sizeof(hex), payload, payload_len);
			LOG_WRN_RATELIMIT("unmatched topic 1 frame from host "
					  "(%u B): %s", payload_len, hex);
		}
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

/*
 * Module boot banner: proof that the module finished booting. The
 * provisioning flow waits for it before its first AT command, because talking
 * to the module while it is still starting up is unreliable.
 */
static void hook_4g_note_boot_banner(const uint8_t *buf, uint16_t len)
{
	if (!hook_4g_modem_boot_seen_flag &&
	    hook_4g_contains(buf, len, "WH-LTE-7S1")) {
		hook_4g_modem_boot_seen_flag = true;
		LOG_INF("4G module boot banner seen (module is up)");
	}
}

static void hook_4g_process_block(const struct hook_4g_rx_block *block)
{
	const uint8_t *buf = block->data;
	uint16_t len = block->len;
	uint8_t topic = 0U;
	uint8_t prefix_len;

	/*
	 * Drop the line noise a module restart puts on the link: one or two
	 * 0x00 bytes (with a framing error at the UART). No valid frame looks
	 * like this - an MQTT payload carries a "<num>," prefix and an AT
	 * answer carries text - so filtering it cannot hide real data, while
	 * letting it through would pollute the AT answer being collected.
	 */
	if ((len <= 2U) && ((len == 0U) || ((buf[0] == 0x00U) &&
					    ((len == 1U) || (buf[1] == 0x00U))))) {
		return;
	}

	if (hook_4g_hex_echo_enabled) {
		hook_4g_hex_dump(buf, len);
	}

	/*
	 * HTTPD mode: the module streams the HTTP response body over the serial
	 * line. Hand it to the OTA download session - but not while a synchronous
	 * AT exchange is running, because switching back to MQTT needs that.
	 */
	if (hook_4g_http_mode && !hook_4g_at_active) {
		if (hook_ota_http_is_armed()) {
			(void)hook_ota_http_feed(buf, len);
		} else {
			/*
			 * Not part of a response: either data that arrived
			 * before the trigger, or the traffic of the module
			 * restarting into/out of HTTPD mode. The boot banner
			 * has to be recognised here as well - the provisioning
			 * flow waits for it, and it is the one line that makes a
			 * module restart visible in the log.
			 */
			hook_4g_note_boot_banner(buf, len);
			LOG_DBG("HTTP OTA: dropped %u B outside a response", len);
		}
		return;
	}

	hook_4g_note_boot_banner(buf, len);

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

/*
 * 8 KiB: during an HTTP OTA this thread parses the response head and calls
 * flash_img_buffered_write() for every received block, on top of the usual
 * frame routing - keep the margin rather than discover it at run time.
 */
K_THREAD_STACK_DEFINE(hook_4g_stack, 8192);
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
