#ifndef RFID_UPLINK_PROTOCOL_H_
#define RFID_UPLINK_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#define RFID_UPLINK_MAGIC 0xA6U
#define RFID_UPLINK_VERSION 0x01U
#define RFID_UPLINK_RECORD_SIZE 6U
#define RFID_UPLINK_MAX_TAGS 10U
#define RFID_UPLINK_MAX_FRAME_SIZE (10U + RFID_UPLINK_RECORD_SIZE * RFID_UPLINK_MAX_TAGS)
#define RFID_UPLINK_FRAME_FLAGS_NONE 0x00U
#define RFID_UPLINK_FLAG_LOW_BATTERY 0x01U

struct rfid_uplink_tag {
	uint32_t uid;
	uint8_t rssi;  /* dB */
	uint8_t flags; /* bit 0: low battery */
};

size_t rfid_uplink_encode(uint16_t sequence,
			 const struct rfid_uplink_tag *tags, uint8_t count,
			 uint8_t *frame, size_t capacity);

#endif
