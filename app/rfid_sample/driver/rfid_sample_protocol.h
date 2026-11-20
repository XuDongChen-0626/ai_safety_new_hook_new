#ifndef RFID_SAMPLE_PROTOCOL_H_
#define RFID_SAMPLE_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RFID_SAMPLE_PROTOCOL_MAX_FRAME_SIZE 25U
#define RFID_SAMPLE_PROTOCOL_MAX_REQUEST_SIZE 8U
#define RFID_SAMPLE_PROTOCOL_DEFAULT_FRAME_TIMEOUT_MS 1000U
#define RFID_SAMPLE_BATTERY_LEVEL_LOW 0x18U
#define RFID_SAMPLE_BATTERY_LEVEL_NORMAL 0x21U

/*
 * Diagnostic record only: tag checksums are intentionally not verified.
 * The last frame byte is consumed and preserved, but NEVER verified.
 * Neither successful framing nor time_fields_valid authenticates a tag.
 * No year base, millisecond byte order or timezone is assumed.
 */
struct rfid_sample_tag_record {
	uint32_t uid;
	uint8_t address;
	uint8_t rssi;    /* Signal strength in dB. */
	uint8_t battery; /* RFID_SAMPLE_BATTERY_LEVEL_* raw code. */
	uint8_t antenna;
	uint8_t additional_type;
	uint8_t checksum;
	bool checksum_verified;
	bool has_device_time;
	bool time_fields_valid;
	uint8_t device_time[8];
	uint8_t raw_frame[RFID_SAMPLE_PROTOCOL_MAX_FRAME_SIZE];
	uint8_t frame_size;
};

/* Counters saturate at UINT32_MAX. frames counts diagnostic records. */
struct rfid_sample_protocol_stats {
	uint32_t frames;
	/* Bytes discarded, including rejected candidates and timed-out data. */
	uint32_t noise_bytes;
	/* Rejected sync candidates, including expired partial candidates. */
	uint32_t sync_losses;
	uint32_t length_errors;
	uint32_t tag_length_errors;
	uint32_t unsupported_messages;
	uint32_t partial_timeouts;
	uint32_t time_field_errors;
	uint32_t checksum_errors;
};

struct rfid_sample_response {
	uint8_t command;
	uint8_t sequence;
	uint8_t address;
	uint8_t device_error;
	uint8_t mode;
	bool has_mode;
};

/* Same borrowed lifetime and serialized, nonblocking context as record_cb. */
typedef void (*rfid_sample_protocol_response_cb)(
	const struct rfid_sample_response *response, void *user_data);

/*
 * One parser per independent byte stream. Fields are public only to permit
 * caller-owned/static allocation; use the API instead of editing them.
 * Per-byte timestamps preserve absolute candidate age after resynchronizing.
 * There is no allocation and at most 25 buffered stream bytes per instance.
 */
struct rfid_sample_protocol {
	uint8_t buffer[RFID_SAMPLE_PROTOCOL_MAX_FRAME_SIZE];
	uint64_t received_ms[RFID_SAMPLE_PROTOCOL_MAX_FRAME_SIZE];
	size_t buffered;
	uint32_t frame_timeout_ms;
	struct rfid_sample_protocol_stats stats;
	rfid_sample_protocol_response_cb response_callback;
	void *response_user_data;
};

/*
 * Invoked synchronously by feed(), in its caller's context. record is borrowed
 * and valid only until this callback returns; copy it to retain it. Callbacks
 * must be bounded/nonblocking and must not call this instance's APIs again.
 * Different instances are independent. All access to one instance (including
 * stats reads) must be serialized by its owner; there is no internal locking.
 */
typedef void (*rfid_sample_protocol_record_cb)(const struct rfid_sample_tag_record *record,
					void *user_data);

/* Clears all state and statistics. A zero timeout selects the default above. */
void rfid_sample_protocol_init(struct rfid_sample_protocol *protocol,
			uint32_t frame_timeout_ms);

/* Discards a partial frame; preserves statistics, timeout and response callback. */
void rfid_sample_protocol_reset(struct rfid_sample_protocol *protocol);

/* Only structurally valid 0x22 responses with payload-XOR BCC verified invoke
 * this callback. init() clears registration; reset() preserves it.
 */
void rfid_sample_protocol_set_response_callback(struct rfid_sample_protocol *protocol,
					rfid_sample_protocol_response_cb callback,
					void *user_data);

/* Encodes 0x39/0x3b requests or a 0x22 mode query; returns 0 without writing on
 * unsupported commands, NULL output or insufficient capacity.
 */
size_t rfid_sample_protocol_make_request(uint8_t command, uint8_t address,
				       uint8_t *frame, size_t capacity);

/*
 * Feed arbitrarily split or concatenated input. now_ms is a monotonic time
 * supplied by the caller; all bytes in a call have this arrival timestamp.
 * Both idle time and total assembly age use frame_timeout_ms, with expiration
 * at >= the limit. A backwards clock value also expires a pending candidate.
 * Call expire() periodically when no bytes arrive. len == 0 also polls expiry.
 * A NULL callback still parses/counts records but discards their delivery.
 * NULL protocol, or NULL data with nonzero len, is ignored safely.
 */
void rfid_sample_protocol_feed(struct rfid_sample_protocol *protocol, const uint8_t *data,
			size_t len, uint64_t now_ms,
			rfid_sample_protocol_record_cb callback, void *user_data);

/* Expires pending data only; never emits a record. */
void rfid_sample_protocol_expire(struct rfid_sample_protocol *protocol, uint64_t now_ms);

/* Copies a consistent snapshot when serialized with feed/reset/expire. */
void rfid_sample_protocol_get_stats(const struct rfid_sample_protocol *protocol,
			     struct rfid_sample_protocol_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* RFID_SAMPLE_PROTOCOL_H_ */
