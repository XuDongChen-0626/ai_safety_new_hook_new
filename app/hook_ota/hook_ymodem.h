/*
 * hook_ymodem.h - Ymodem receiver used by the MQTT/4G OTA path.
 *
 * Ported from craner_bootloader01/application/ota_ymodem_protocol.c with the
 * protocol behaviour unchanged, so that the existing host tool
 * (tool-ota-mqtt-ymodem, src/ymodem.py) works without modifications:
 *
 *   receiver -> 'C'                                     (CRC mode request)
 *   receiver -> ACK + 'C'   after block 0 (file info)
 *   receiver -> ACK / NAK   after every 1 KiB data packet
 *   receiver -> NAK         on the first EOT
 *   receiver -> ACK + 'C'   on the second EOT
 *   receiver -> ACK         after the final empty block 0
 */
#ifndef HOOK_YMODEM_H
#define HOOK_YMODEM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOOK_YMODEM_SOH          0x01U
#define HOOK_YMODEM_STX          0x02U
#define HOOK_YMODEM_EOT          0x04U
#define HOOK_YMODEM_ACK          0x06U
#define HOOK_YMODEM_NAK          0x15U
#define HOOK_YMODEM_CAN          0x18U
#define HOOK_YMODEM_CRC_REQUEST  0x43U /* 'C' */

#define HOOK_YMODEM_FILE_NAME_MAX   64U
#define HOOK_YMODEM_PACKET_DATA_MAX 1024U

typedef enum {
	HOOK_YMODEM_OK = 0,
	HOOK_YMODEM_ERR_PARAM = -1,
	HOOK_YMODEM_ERR_SEND = -2,
	HOOK_YMODEM_ERR_SEQUENCE = -3,
	HOOK_YMODEM_ERR_CRC = -4,
	HOOK_YMODEM_ERR_PACKET = -5,
	HOOK_YMODEM_ERR_CALLBACK = -6,
	HOOK_YMODEM_ERR_ABORTED = -7,
	HOOK_YMODEM_ERR_TIMEOUT = -8,
} hook_ymodem_status_t;

typedef enum {
	HOOK_YMODEM_RX_IDLE = 0,
	HOOK_YMODEM_RX_FIND_HEADER,
	HOOK_YMODEM_RX_READ_PACKET_SEQ,
	HOOK_YMODEM_RX_READ_PACKET_SEQ_INV,
	HOOK_YMODEM_RX_RECV_FILE_INFO,
	HOOK_YMODEM_RX_RECV_FILE_DATA,
	HOOK_YMODEM_RX_RECV_FINISH_PACKET,
	HOOK_YMODEM_RX_DONE,
	HOOK_YMODEM_RX_ABORTED,
} hook_ymodem_rx_state_t;

typedef struct {
	char name[HOOK_YMODEM_FILE_NAME_MAX];
	uint32_t size;
	uint32_t bytes_received;
	uint8_t size_valid;
} hook_ymodem_file_info_t;

typedef int (*hook_ymodem_send_fn_t)(const uint8_t *data, uint16_t len, void *user);
typedef int (*hook_ymodem_file_begin_fn_t)(const hook_ymodem_file_info_t *file,
					   void *user);
typedef int (*hook_ymodem_file_data_fn_t)(const uint8_t *data, uint16_t len,
					  uint32_t offset, void *user);
typedef void (*hook_ymodem_file_end_fn_t)(const hook_ymodem_file_info_t *file,
					  void *user);
typedef void (*hook_ymodem_abort_fn_t)(hook_ymodem_status_t reason, void *user);

typedef struct {
	hook_ymodem_send_fn_t send;
	hook_ymodem_file_begin_fn_t on_file_begin;
	hook_ymodem_file_data_fn_t on_file_data;
	hook_ymodem_file_end_fn_t on_file_end;
	hook_ymodem_abort_fn_t on_abort;
	void *user;
	uint8_t max_errors;
	uint32_t packet_timeout_ms;
} hook_ymodem_config_t;

void hook_ymodem_init(const hook_ymodem_config_t *config);

/* Reset the session and send the initial CRC request ('C'). */
hook_ymodem_status_t hook_ymodem_start_receive(void);

void hook_ymodem_reset(void);

/* Feed one byte received from the OTA channel. */
hook_ymodem_status_t hook_ymodem_input_byte(uint8_t byte);

/* Periodic call: enforces the per-packet timeout (NAK / abort). */
hook_ymodem_status_t hook_ymodem_tick(uint32_t now_ms);

/* Send one raw control byte ('C' / ACK / NAK / CAN) to the channel. */
hook_ymodem_status_t hook_ymodem_send_control(uint8_t control);

hook_ymodem_rx_state_t hook_ymodem_get_state(void);
const hook_ymodem_file_info_t *hook_ymodem_get_file_info(void);
bool hook_ymodem_is_busy(void);
bool hook_ymodem_is_done(void);

/* True while the receiver waits for a packet header, i.e. the sender may be
 * waiting for our 'C' (used by the service to re-arm the CRC request).
 */
bool hook_ymodem_waiting_for_packet(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_YMODEM_H */
