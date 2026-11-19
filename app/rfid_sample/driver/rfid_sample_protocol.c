#include "rfid_sample_protocol.h"

#include <string.h>

#define RFID_SYNC_FIRST 0x1bU
#define RFID_TAG_COMMAND 0x39U
#define RFID_STOP_COMMAND 0x3bU
#define RFID_QUERY_COMMAND 0x22U
#define RFID_TAG_SEQUENCE 0x01U
#define RFID_MODE_PARAMETER 0x08U
#define RFID_PACKET_SIZE_WITHOUT_TIME 10U
#define RFID_PACKET_SIZE_WITH_TIME 18U
#define RFID_TAG_DATA_SIZE 8U
#define RFID_FRAME_OVERHEAD 7U

static void rfid_sample_protocol_count(uint32_t *counter)
{
	if (*counter != UINT32_MAX) {
		(*counter)++;
	}
}

static void rfid_sample_protocol_count_noise(struct rfid_sample_protocol *protocol,
				     size_t count)
{
	/* count never exceeds the fixed frame buffer size. */
	if ((uint32_t)count > UINT32_MAX - protocol->stats.noise_bytes) {
		protocol->stats.noise_bytes = UINT32_MAX;
	} else {
		protocol->stats.noise_bytes += (uint32_t)count;
	}
}

static void rfid_sample_protocol_drop_prefix(struct rfid_sample_protocol *protocol,
				     size_t count)
{
	const size_t remaining = protocol->buffered - count;

	if (remaining != 0U) {
		memmove(protocol->buffer, protocol->buffer + count, remaining);
		memmove(protocol->received_ms, protocol->received_ms + count,
			remaining * sizeof(protocol->received_ms[0]));
	}
	protocol->buffered = remaining;
}

static void rfid_sample_protocol_reject_candidate(struct rfid_sample_protocol *protocol)
{
	rfid_sample_protocol_count(&protocol->stats.sync_losses);
	rfid_sample_protocol_count_noise(protocol, 1U);
	/* Keep every remaining byte: another 1b may overlap this candidate. */
	rfid_sample_protocol_drop_prefix(protocol, 1U);
}

static bool rfid_sample_protocol_time_fields_valid(const uint8_t time[8])
{
	/* Only the established direct-value month/day/hour/minute/second fields.
	 * Do not guess a year base, calendar leap-year rule or millisecond order.
	 */
	return time[1] >= 1U && time[1] <= 12U &&
	       time[2] >= 1U && time[2] <= 31U &&
	       time[3] <= 23U && time[4] <= 59U && time[5] <= 59U;
}

static void rfid_sample_protocol_decode(const uint8_t *frame, size_t frame_size,
				struct rfid_sample_tag_record *record)
{
	memset(record, 0, sizeof(*record));
	record->uid = ((uint32_t)frame[10] << 24) |
		      ((uint32_t)frame[11] << 16) |
		      ((uint32_t)frame[12] << 8) | (uint32_t)frame[13];
	record->address = frame[3];
	record->rssi = frame[7];
	record->pc = frame[8];
	record->battery = frame[9];
	record->antenna = frame[14];
	record->additional_type = frame[frame_size - 2U];
	record->checksum = frame[frame_size - 1U];
	record->checksum_verified = false;
	record->has_device_time = frame_size == RFID_SAMPLE_PROTOCOL_MAX_FRAME_SIZE;
	if (record->has_device_time) {
		memcpy(record->device_time, frame + 15U,
		       sizeof(record->device_time));
		record->time_fields_valid =
			rfid_sample_protocol_time_fields_valid(record->device_time);
	}
	memcpy(record->raw_frame, frame, frame_size);
	record->frame_size = (uint8_t)frame_size;
}

static uint8_t rfid_sample_protocol_checksum(const uint8_t *data, size_t size)
{
	uint8_t checksum = 0U;

	for (size_t i = 0U; i < size; i++) {
		checksum ^= data[i];
	}
	return checksum;
}

static bool rfid_sample_protocol_decode_response(const uint8_t *frame,
						uint16_t packet_size,
						struct rfid_sample_response *response)
{
	memset(response, 0, sizeof(*response));
	response->command = frame[1];
	response->sequence = frame[2];
	response->address = frame[3];
	if (packet_size == 2U && frame[6] == RFID_MODE_PARAMETER && frame[7] <= 2U) {
		response->has_mode = true;
		response->mode = frame[7];
		return true;
	}
	if (packet_size == 1U && frame[6] >= 0x80U && frame[6] <= 0x82U) {
		response->device_error = frame[6];
		return true;
	}
	return false;
}

static void rfid_sample_protocol_process(struct rfid_sample_protocol *protocol,
				  rfid_sample_protocol_record_cb callback,
				  void *user_data)
{
	/* Each iteration consumes a byte/frame or returns for more input. */
	while (protocol->buffered != 0U) {
		uint16_t packet_size;
		size_t frame_size;
		bool is_tag;
		struct rfid_sample_tag_record record;

		if (protocol->buffer[0] != RFID_SYNC_FIRST) {
			rfid_sample_protocol_count_noise(protocol, 1U);
			rfid_sample_protocol_drop_prefix(protocol, 1U);
			continue;
		}
		if (protocol->buffered < 2U) {
			return;
		}
		is_tag = protocol->buffer[1] == RFID_TAG_COMMAND;
		if (!is_tag && protocol->buffer[1] != RFID_QUERY_COMMAND) {
			rfid_sample_protocol_count(&protocol->stats.unsupported_messages);
			rfid_sample_protocol_reject_candidate(protocol);
			continue;
		}
		if (protocol->buffered < 3U) {
			return;
		}
		if (is_tag && protocol->buffer[2] != RFID_TAG_SEQUENCE) {
			rfid_sample_protocol_count(&protocol->stats.unsupported_messages);
			rfid_sample_protocol_reject_candidate(protocol);
			continue;
		}
		if (protocol->buffered < 6U) {
			return;
		}
		packet_size = (uint16_t)protocol->buffer[4] |
			      (uint16_t)((uint16_t)protocol->buffer[5] << 8);
		if ((is_tag && packet_size != RFID_PACKET_SIZE_WITHOUT_TIME &&
		     packet_size != RFID_PACKET_SIZE_WITH_TIME) ||
		    (!is_tag && packet_size != 1U && packet_size != 2U)) {
			rfid_sample_protocol_count(&protocol->stats.length_errors);
			rfid_sample_protocol_reject_candidate(protocol);
			continue;
		}
		if (protocol->buffered < 7U) {
			return;
		}
		if (is_tag && protocol->buffer[6] != RFID_TAG_DATA_SIZE) {
			rfid_sample_protocol_count(&protocol->stats.tag_length_errors);
			rfid_sample_protocol_reject_candidate(protocol);
			continue;
		}
		/* Accepted payload sizes are bounded by 18 bytes. */
		frame_size = (size_t)packet_size + RFID_FRAME_OVERHEAD;
		if (protocol->buffered < frame_size) {
			return;
		}
		if (!is_tag) {
			struct rfid_sample_response response;

			if (rfid_sample_protocol_checksum(protocol->buffer + 6U, packet_size) !=
			    protocol->buffer[frame_size - 1U]) {
				rfid_sample_protocol_count(&protocol->stats.checksum_errors);
				rfid_sample_protocol_reject_candidate(protocol);
				continue;
			}
			if (!rfid_sample_protocol_decode_response(protocol->buffer, packet_size,
							  &response)) {
				rfid_sample_protocol_count(&protocol->stats.unsupported_messages);
				rfid_sample_protocol_reject_candidate(protocol);
				continue;
			}
			rfid_sample_protocol_drop_prefix(protocol, frame_size);
			if (protocol->response_callback != NULL) {
				protocol->response_callback(&response, protocol->response_user_data);
			}
			continue;
		}

		rfid_sample_protocol_decode(protocol->buffer, frame_size, &record);
		rfid_sample_protocol_drop_prefix(protocol, frame_size);
		rfid_sample_protocol_count(&protocol->stats.frames);
		if (record.has_device_time && !record.time_fields_valid) {
			rfid_sample_protocol_count(&protocol->stats.time_field_errors);
		}
		if (callback != NULL) {
			callback(&record, user_data);
		}
	}
}

void rfid_sample_protocol_init(struct rfid_sample_protocol *protocol,
			uint32_t frame_timeout_ms)
{
	if (protocol == NULL) {
		return;
	}

	memset(protocol, 0, sizeof(*protocol));
	protocol->frame_timeout_ms = frame_timeout_ms == 0U ?
		RFID_SAMPLE_PROTOCOL_DEFAULT_FRAME_TIMEOUT_MS : frame_timeout_ms;
}

void rfid_sample_protocol_reset(struct rfid_sample_protocol *protocol)
{
	if (protocol == NULL) {
		return;
	}

	protocol->buffered = 0U;
	memset(protocol->buffer, 0, sizeof(protocol->buffer));
	memset(protocol->received_ms, 0, sizeof(protocol->received_ms));
}

void rfid_sample_protocol_set_response_callback(struct rfid_sample_protocol *protocol,
					rfid_sample_protocol_response_cb callback,
					void *user_data)
{
	if (protocol == NULL) {
		return;
	}
	protocol->response_callback = callback;
	protocol->response_user_data = user_data;
}

size_t rfid_sample_protocol_make_request(uint8_t command, uint8_t address,
				       uint8_t *frame, size_t capacity)
{
	uint8_t payload_size;
	uint8_t sequence;
	size_t frame_size;

	if (command == RFID_TAG_COMMAND || command == RFID_STOP_COMMAND) {
		payload_size = 0U;
		sequence = RFID_TAG_SEQUENCE;
	} else if (command == RFID_QUERY_COMMAND) {
		payload_size = 1U;
		sequence = 0U;
	} else {
		return 0U;
	}
	frame_size = (size_t)payload_size + RFID_FRAME_OVERHEAD;
	if (frame == NULL || capacity < frame_size) {
		return 0U;
	}
	frame[0] = RFID_SYNC_FIRST;
	frame[1] = command;
	frame[2] = sequence;
	frame[3] = address;
	frame[4] = payload_size;
	frame[5] = 0U;
	if (payload_size != 0U) {
		frame[6] = RFID_MODE_PARAMETER;
	}
	frame[frame_size - 1U] = rfid_sample_protocol_checksum(frame + 6U, payload_size);
	return frame_size;
}

void rfid_sample_protocol_expire(struct rfid_sample_protocol *protocol, uint64_t now_ms)
{
	uint64_t first_ms;
	uint64_t last_ms;

	if (protocol == NULL || protocol->buffered == 0U) {
		return;
	}

	first_ms = protocol->received_ms[0];
	last_ms = protocol->received_ms[protocol->buffered - 1U];
	/* Test order avoids subtraction underflow when the supplied clock resets. */
	if (now_ms < first_ms || now_ms < last_ms ||
	    now_ms - first_ms >= protocol->frame_timeout_ms ||
	    now_ms - last_ms >= protocol->frame_timeout_ms) {
		rfid_sample_protocol_count(&protocol->stats.partial_timeouts);
		rfid_sample_protocol_count(&protocol->stats.sync_losses);
		rfid_sample_protocol_count_noise(protocol, protocol->buffered);
		rfid_sample_protocol_reset(protocol);
	}
}

void rfid_sample_protocol_feed(struct rfid_sample_protocol *protocol, const uint8_t *data,
			size_t len, uint64_t now_ms,
			rfid_sample_protocol_record_cb callback, void *user_data)
{
	if (protocol == NULL || (data == NULL && len != 0U)) {
		return;
	}

	rfid_sample_protocol_expire(protocol, now_ms);
	for (size_t i = 0U; i < len; i++) {
		/* A complete 25-byte candidate is consumed by process() immediately.
		 * Retain a defensive bound in case future formats change that rule.
		 */
		if (protocol->buffered == RFID_SAMPLE_PROTOCOL_MAX_FRAME_SIZE) {
			rfid_sample_protocol_count(&protocol->stats.length_errors);
			rfid_sample_protocol_reject_candidate(protocol);
		}
		protocol->buffer[protocol->buffered] = data[i];
		protocol->received_ms[protocol->buffered] = now_ms;
		protocol->buffered++;
		rfid_sample_protocol_process(protocol, callback, user_data);
	}
}

void rfid_sample_protocol_get_stats(const struct rfid_sample_protocol *protocol,
			     struct rfid_sample_protocol_stats *stats)
{
	if (stats == NULL) {
		return;
	}
	if (protocol == NULL) {
		memset(stats, 0, sizeof(*stats));
		return;
	}

	*stats = protocol->stats;
}
