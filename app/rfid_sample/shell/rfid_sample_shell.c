#include "rfid_sample.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_RFID_UPLINK)
#include "rfid_uplink.h"
#endif

#define RFID_SAMPLE_SHELL_QUEUE_DEPTH 8U
#define RFID_SAMPLE_SHELL_STACK_SIZE 2048U
#define RFID_SAMPLE_SHELL_PRIORITY 12

struct rfid_sample_shell_item {
	atomic_val_t session;
	struct rfid_sample_diagnostic diagnostic;
};

K_MSGQ_DEFINE(rfid_sample_shell_queue, sizeof(struct rfid_sample_shell_item),
	     RFID_SAMPLE_SHELL_QUEUE_DEPTH, 4);
K_MUTEX_DEFINE(rfid_sample_shell_lock);

static atomic_t rfid_sample_shell_enabled;
static atomic_t rfid_sample_shell_session;
static atomic_t rfid_sample_shell_drops;
static const struct shell *rfid_sample_shell_output;

static void rfid_sample_shell_hex(const uint8_t *bytes, size_t count,
				char *text, size_t capacity)
{
	static const char digits[] = "0123456789ABCDEF";
	size_t used = 0U;

	if (capacity == 0U) {
		return;
	}
	for (size_t i = 0U; (i < count) && (capacity - used >= 3U); i++) {
		text[used++] = digits[bytes[i] >> 4];
		text[used++] = digits[bytes[i] & 0x0fU];
	}
	text[used] = '\0';
}

static void rfid_sample_shell_observe(const struct zbus_channel *chan)
{
	struct rfid_sample_shell_item item;

	item.session = atomic_get(&rfid_sample_shell_session);
	if (atomic_get(&rfid_sample_shell_enabled) == 0) {
		return;
	}

	memcpy(&item.diagnostic, zbus_chan_const_msg(chan),
	       sizeof(item.diagnostic));
	if ((atomic_get(&rfid_sample_shell_enabled) == 0) ||
	    (item.session != atomic_get(&rfid_sample_shell_session))) {
		return;
	}

	if (k_msgq_put(&rfid_sample_shell_queue, &item, K_NO_WAIT) != 0) {
		atomic_inc(&rfid_sample_shell_drops);
	}
}

ZBUS_LISTENER_DEFINE(rfid_sample_shell_listener, rfid_sample_shell_observe);
ZBUS_CHAN_ADD_OBS(rfid_sample_diagnostic_chan, rfid_sample_shell_listener, 0);

static void rfid_sample_shell_print(const struct shell *sh,
				  const struct rfid_sample_diagnostic *sample)
{
	const struct rfid_sample_tag_record *tag = &sample->tag;

	shell_print(sh, "{\"uid\":\"%08x\",\"rssi\":%u,\"bat\":%u}",
		    (unsigned int)tag->uid, (unsigned int)tag->rssi,
		    (unsigned int)tag->battery);
}

static void rfid_sample_shell_thread(void *arg1, void *arg2, void *arg3)
{
	struct rfid_sample_shell_item item;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		if (k_msgq_get(&rfid_sample_shell_queue, &item, K_FOREVER) != 0) {
			continue;
		}

		k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
		if ((atomic_get(&rfid_sample_shell_enabled) != 0) &&
		    (item.session == atomic_get(&rfid_sample_shell_session)) &&
		    (rfid_sample_shell_output != NULL)) {
			rfid_sample_shell_print(rfid_sample_shell_output, &item.diagnostic);
		}
		k_mutex_unlock(&rfid_sample_shell_lock);
	}
}

K_THREAD_DEFINE(rfid_sample_shell_tid, RFID_SAMPLE_SHELL_STACK_SIZE,
		rfid_sample_shell_thread, NULL, NULL, NULL,
		RFID_SAMPLE_SHELL_PRIORITY, 0, 0);

static int rfid_sample_shell_stream(const struct shell *sh, bool enable)
{
	k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
	atomic_clear(&rfid_sample_shell_enabled);
	atomic_inc(&rfid_sample_shell_session);
	k_msgq_purge(&rfid_sample_shell_queue);
	rfid_sample_shell_output = enable ? sh : NULL;
	if (enable) {
		atomic_set(&rfid_sample_shell_enabled, 1);
	}
	shell_print(sh, "{\"stream\":%s}", enable ? "true" : "false");
	k_mutex_unlock(&rfid_sample_shell_lock);
	return 0;
}

static int rfid_sample_shell_status(const struct shell *sh)
{
	struct rfid_sample_status status = {0};

	rfid_sample_get_status(&status);
	uint64_t parse_errors = (uint64_t)status.protocol.sync_losses +
				status.protocol.time_field_errors;

	k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
	shell_print(sh, "{\"ready\":%s,\"uart\":\"%s\",\"baud\":%" PRIu32
		    ",\"stream\":%s,\"err\":%d,"
		    "\"rx\":%" PRIu32 ",\"frames\":%" PRIu32
		    ",\"uart_err\":%" PRIu32 ",\"parse_err\":%" PRIu64
		    ",\"bcc_err\":%" PRIu32
		    ",\"noise\":%" PRIu32 ",\"rx_drop\":%" PRIu32
		    ",\"pub_drop\":%" PRIu32 ",\"print_drop\":%" PRIu32 "}",
		    status.ready ? "true" : "false",
		    status.uart_name, status.baudrate,
		    atomic_get(&rfid_sample_shell_enabled) != 0 ? "true" : "false",
		    status.init_error, status.rx_bytes, status.protocol.frames,
		    status.uart_errors, parse_errors, status.protocol.checksum_errors,
		    status.protocol.noise_bytes,
		    status.rx_dropped_bytes,
		    status.publish_drops,
		    (uint32_t)atomic_get(&rfid_sample_shell_drops));
	k_mutex_unlock(&rfid_sample_shell_lock);
	return 0;
}

static int rfid_sample_shell_show(const struct shell *sh)
{
	struct rfid_sample_diagnostic sample;
	int ret;

	ret = zbus_chan_read(&rfid_sample_diagnostic_chan, &sample, K_MSEC(50));
	k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
	if (ret != 0) {
		shell_print(sh, "{\"error\":%d}", ret);
	} else if (sample.sequence == 0U) {
		shell_print(sh, "{\"tag\":null}");
	} else {
		rfid_sample_shell_print(sh, &sample);
	}
	k_mutex_unlock(&rfid_sample_shell_lock);
	return ret;
}

static int rfid_sample_shell_raw(const struct shell *sh)
{
	struct rfid_sample_diagnostic sample;
	char raw[RFID_SAMPLE_PROTOCOL_MAX_FRAME_SIZE * 2U + 1U];
	int ret;

	ret = zbus_chan_read(&rfid_sample_diagnostic_chan, &sample, K_MSEC(50));
	k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
	if (ret != 0) {
		shell_print(sh, "{\"error\":%d}", ret);
	} else if (sample.sequence == 0U) {
		shell_print(sh, "{\"raw\":null}");
	} else {
		rfid_sample_shell_hex(sample.tag.raw_frame, sample.tag.frame_size,
				     raw, sizeof(raw));
		shell_print(sh, "{\"raw\":\"%s\",\"len\":%u}", raw,
			    (unsigned int)sample.tag.frame_size);
	}
	k_mutex_unlock(&rfid_sample_shell_lock);
	return ret;
}

static int rfid_sample_shell_invalid(const struct shell *sh)
{
	k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
	shell_print(sh, "{\"error\":%d}", -EINVAL);
	k_mutex_unlock(&rfid_sample_shell_lock);
	return -EINVAL;
}

static bool rfid_sample_shell_address(const char *text, uint8_t *address)
{
	unsigned int value = 0U;
	unsigned int base = 10U;

	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		base = 16U;
		text += 2;
	}
	if (*text == '\0') {
		return false;
	}
	while (*text != '\0') {
		unsigned int digit;

		if (*text >= '0' && *text <= '9') {
			digit = (unsigned int)(*text - '0');
		} else if (base == 16U && *text >= 'a' && *text <= 'f') {
			digit = (unsigned int)(*text - 'a') + 10U;
		} else if (base == 16U && *text >= 'A' && *text <= 'F') {
			digit = (unsigned int)(*text - 'A') + 10U;
		} else {
			return false;
		}
		value = value * base + digit;
		if (value > UINT8_MAX) {
			return false;
		}
		text++;
	}
	*address = (uint8_t)value;
	return true;
}

static int rfid_sample_shell_mode(const struct shell *sh, uint8_t address)
{
	static const char *const names[] = {"auto", "command", "trigger"};
	struct rfid_sample_response response = {0};
	int rc = rfid_sample_get_mode(address, &response);

	k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
	if (rc != 0) {
		shell_print(sh, "{\"error\":%d,\"device_error\":%u,\"read_may_stop\":true}",
			    rc, (unsigned int)response.device_error);
	} else {
		shell_print(sh, "{\"mode\":%u,\"name\":\"%s\",\"addr\":%u,"
			    "\"read_may_stop\":true}", (unsigned int)response.mode,
			    response.mode < ARRAY_SIZE(names) ? names[response.mode] : "unknown",
			    (unsigned int)response.address);
	}
	k_mutex_unlock(&rfid_sample_shell_lock);
	return rc;
}

static int rfid_sample_shell_read(const struct shell *sh, bool start, uint8_t address)
{
	int rc = rfid_sample_inventory(start, address);

	k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
	if (rc != 0) {
		shell_print(sh, "{\"error\":%d,\"confirmed\":false}", rc);
	} else {
		shell_print(sh, "{\"read\":\"%s\",\"addr\":%u,\"tx\":\"queued\","
			    "\"confirmed\":false}", start ? "start" : "stop",
			    (unsigned int)address);
	}
	k_mutex_unlock(&rfid_sample_shell_lock);
	return rc;
}

static int rfid_sample_shell_baud(const struct shell *sh, const char *text)
{
	uint32_t baudrate;

	if (strcmp(text, "9600") == 0) {
		baudrate = 9600U;
	} else if (strcmp(text, "115200") == 0) {
		baudrate = 115200U;
	} else {
		return rfid_sample_shell_invalid(sh);
	}

	int rc = rfid_sample_set_baudrate(baudrate);

	k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
	if (rc == 0) {
		shell_print(sh, "{\"uart\":\"baud\",\"value\":%" PRIu32 "}",
			    baudrate);
	} else {
		shell_print(sh, "{\"error\":%d}", rc);
	}
	k_mutex_unlock(&rfid_sample_shell_lock);
	return rc;
}

#if defined(CONFIG_RFID_UPLINK)
static int rfid_sample_shell_uplink_status(const struct shell *sh)
{
	struct rfid_uplink_status status = {0};

	rfid_uplink_get_status(&status);
	k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
	shell_print(sh, "{\"uplink\":true,\"channel\":4,\"period_ms\":%u,"
		    "\"pending\":%u,\"sent\":%" PRIu32 ",\"empty\":%" PRIu32
		    ",\"event_drop\":%" PRIu32 ",\"tag_overflow\":%" PRIu32
		    ",\"deferred\":%" PRIu32 ",\"send_error\":%" PRIu32
		    ",\"last_error\":%d,\"next_seq\":%u}",
		    (unsigned int)CONFIG_RFID_UPLINK_PERIOD_MS,
		    (unsigned int)status.pending_tags, status.windows_sent,
		    status.empty_windows_sent, status.event_drops,
		    status.tag_overflows, status.deferred_windows,
		    status.send_errors, status.last_error,
		    (unsigned int)status.next_sequence);
	k_mutex_unlock(&rfid_sample_shell_lock);
	return 0;
}
#endif

static int rfid_sample_shell_command(const struct shell *sh, size_t argc,
				    char **argv)
{
	char tokens[3][12] = {0};
	size_t count = 0U;
	uint8_t address = 0U;
	const char *cursor = argc > 1U ? argv[1] : "";

	if (argc > 2U) {
		return rfid_sample_shell_invalid(sh);
	}
	while (*cursor != '\0') {
		cursor += strspn(cursor, " \t\r\n");
		if (*cursor == '\0') {
			break;
		}
		size_t length = strcspn(cursor, " \t\r\n");

		if (count == ARRAY_SIZE(tokens) || length >= sizeof(tokens[0])) {
			return rfid_sample_shell_invalid(sh);
		}
		memcpy(tokens[count++], cursor, length);
		cursor += length;
	}

	if (count == 0U || (count == 1U &&
	    (strcmp(tokens[0], "help") == 0 || strcmp(tokens[0], "-h") == 0 ||
	     strcmp(tokens[0], "--help") == 0))) {
		k_mutex_lock(&rfid_sample_shell_lock, K_FOREVER);
		shell_print(sh, "{\"cmd\":[\"stream on\",\"stream off\",\"sample show\","
			    "\"sample raw\",\"status\","
			    "\"uart baud 9600|115200\",\"mode get [addr]\","
			    "\"read start [addr]\",\"read stop [addr]\""
#if defined(CONFIG_RFID_UPLINK)
			    ",\"uplink status\""
#endif
			    "]}");
		k_mutex_unlock(&rfid_sample_shell_lock);
		return 0;
	}
	if (count == 1U && strcmp(tokens[0], "status") == 0) {
		return rfid_sample_shell_status(sh);
	}
	if (count == 2U && strcmp(tokens[0], "sample") == 0 &&
	    strcmp(tokens[1], "show") == 0) {
		return rfid_sample_shell_show(sh);
	}
	if (count == 2U && strcmp(tokens[0], "sample") == 0 &&
	    strcmp(tokens[1], "raw") == 0) {
		return rfid_sample_shell_raw(sh);
	}
	if (count == 2U && strcmp(tokens[0], "stream") == 0) {
		if (strcmp(tokens[1], "on") == 0) {
			return rfid_sample_shell_stream(sh, true);
		}
		if (strcmp(tokens[1], "off") == 0) {
			return rfid_sample_shell_stream(sh, false);
		}
	}
#if defined(CONFIG_RFID_UPLINK)
	if (count == 2U && strcmp(tokens[0], "uplink") == 0 &&
	    strcmp(tokens[1], "status") == 0) {
		return rfid_sample_shell_uplink_status(sh);
	}
#endif
	if (count == 3U && strcmp(tokens[0], "uart") == 0 &&
	    strcmp(tokens[1], "baud") == 0) {
		return rfid_sample_shell_baud(sh, tokens[2]);
	}
	if (count == 2U || count == 3U) {
		if (count == 3U && !rfid_sample_shell_address(tokens[2], &address)) {
			return rfid_sample_shell_invalid(sh);
		}
		if (strcmp(tokens[0], "mode") == 0 && strcmp(tokens[1], "get") == 0) {
			return rfid_sample_shell_mode(sh, address);
		}
		if (strcmp(tokens[0], "read") == 0) {
			if (strcmp(tokens[1], "start") == 0) {
				return rfid_sample_shell_read(sh, true, address);
			}
			if (strcmp(tokens[1], "stop") == 0) {
				return rfid_sample_shell_read(sh, false, address);
			}
		}
	}
	return rfid_sample_shell_invalid(sh);
}

SHELL_CMD_ARG_REGISTER(rfid, NULL, "RFID JSON diagnostics.",
		       rfid_sample_shell_command, 1, SHELL_OPT_ARG_RAW);
