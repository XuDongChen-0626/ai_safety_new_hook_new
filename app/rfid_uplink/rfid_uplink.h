#ifndef RFID_UPLINK_H_
#define RFID_UPLINK_H_

#include <stdint.h>

struct rfid_uplink_status {
	uint16_t next_sequence;
	uint8_t pending_tags;
	uint32_t windows_sent;
	uint32_t empty_windows_sent;
	uint32_t event_drops;
	uint32_t tag_overflows;
	uint32_t deferred_windows;
	uint32_t send_errors;
	int last_error;
};

void rfid_uplink_get_status(struct rfid_uplink_status *status);

#endif
