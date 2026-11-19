#ifndef RFID_SAMPLE_H_
#define RFID_SAMPLE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/zbus/zbus.h>

#include "../driver/rfid_sample_protocol.h"

/* Diagnostic reports only; framing is checked but checksum is NOT verified.
 * Published by the receiver thread, never by an ISR. Consumers must copy the
 * message and must not block the publisher. No tag presence policy is applied.
 */
struct rfid_sample_diagnostic {
	uint64_t received_ms;
	uint64_t published_ms;
	uint32_t sequence;
	uint32_t stream_resets;
	uint16_t source_id;
	struct rfid_sample_tag_record tag;
};

ZBUS_CHAN_DECLARE(rfid_sample_diagnostic_chan);

struct rfid_sample_status {
	const char *uart_name;
	uint32_t baudrate;
	bool ready;
	int init_error;
	uint32_t rx_bytes;
	uint32_t rx_dropped_bytes;
	uint32_t uart_errors;
	uint32_t stream_resets;
	uint32_t publish_drops;
	uint64_t last_received_ms;
	struct rfid_sample_protocol_stats protocol;
};

/* Thread-context diagnostic snapshot. Counters span the current boot.
 * UART counters are sampled independently of the parser snapshot.
 * No control of the producer or access to its private buffers is exposed.
 */
void rfid_sample_get_status(struct rfid_sample_status *status);

/* Thread-only command entry points, never call from a zbus listener/receiver.
 * One request at a time; concurrent callers get -EBUSY. Addresses 0/255 are
 * broadcast and require exactly one reader on the bus. No automatic retries.
 * Mode reads wait up to 100 ms for UART submission and 1000 ms for a reply.
 * They read configured mode, may STOP inventory, and do not auto-resume it.
 * A device failure returns -EIO with device_error populated in response.
 * Inventory success means UART submission only: chapter 8 defines no ACK.
 */
int rfid_sample_get_mode(uint8_t address, struct rfid_sample_response *response);
int rfid_sample_inventory(bool start, uint8_t address);
int rfid_sample_set_baudrate(uint32_t baudrate);

#endif
