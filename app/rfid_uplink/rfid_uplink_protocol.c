#include "rfid_uplink_protocol.h"

#include "hook_cloud_proto.h"

size_t rfid_uplink_encode(uint16_t sequence,
			 const struct rfid_uplink_tag *tags, uint8_t count,
			 uint8_t *frame, size_t capacity)
{
	size_t payload_len = 2U + (size_t)count * RFID_UPLINK_RECORD_SIZE;
	size_t frame_len = 6U + payload_len + 2U;
	size_t offset = 0U;

	if (frame == NULL || count > RFID_UPLINK_MAX_TAGS ||
	    (count > 0U && tags == NULL) || capacity < frame_len) {
		return 0U;
	}

	frame[offset++] = RFID_UPLINK_MAGIC;
	frame[offset++] = RFID_UPLINK_VERSION;
	frame[offset++] = (uint8_t)(sequence & 0xffU);
	frame[offset++] = (uint8_t)(sequence >> 8);
	frame[offset++] = (uint8_t)(payload_len & 0xffU);
	frame[offset++] = (uint8_t)(payload_len >> 8);
	frame[offset++] = count;
	frame[offset++] = RFID_UPLINK_FRAME_FLAGS_NONE;

	for (uint8_t i = 0U; i < count; i++) {
		frame[offset++] = (uint8_t)(tags[i].uid >> 24);
		frame[offset++] = (uint8_t)(tags[i].uid >> 16);
		frame[offset++] = (uint8_t)(tags[i].uid >> 8);
		frame[offset++] = (uint8_t)tags[i].uid;
		frame[offset++] = tags[i].rssi;
		frame[offset++] = tags[i].flags;
	}

	uint16_t crc = hook_cloud_crc16_ccitt(frame, (uint32_t)offset);

	frame[offset++] = (uint8_t)(crc & 0xffU);
	frame[offset++] = (uint8_t)(crc >> 8);
	return offset;
}
