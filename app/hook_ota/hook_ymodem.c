/*
 * hook_ymodem.c - Ymodem receiver (Zephyr port).
 *
 * Behaviour is a 1:1 port of craner_bootloader01/application/
 * ota_ymodem_protocol.c; only the logging calls and the uint types were
 * adapted. Keep the packet/ACK/NAK/EOT sequence unchanged, it is what the
 * host tool (tool-ota-mqtt-ymodem) expects.
 */
#include "hook_ymodem.h"

#include <string.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hook_ymodem, CONFIG_LOG_DEFAULT_LEVEL);

#define YMODEM_SOH_DATA_SIZE   128U
#define YMODEM_STX_DATA_SIZE   1024U
#define YMODEM_DEFAULT_MAX_ERRORS 10U
#define YMODEM_CAN_COUNT_TO_ABORT 2U
#define YMODEM_DEFAULT_PACKET_TIMEOUT_MS 1000U

typedef enum {
	YMODEM_PHASE_WAIT_FILE_INFO = 0,
	YMODEM_PHASE_WAIT_FILE_DATA,
	YMODEM_PHASE_WAIT_FINISH_PACKET,
	YMODEM_PHASE_DONE,
	YMODEM_PHASE_ABORTED,
} ymodem_phase_t;

typedef struct {
	hook_ymodem_config_t config;
	hook_ymodem_rx_state_t rx_state;
	ymodem_phase_t phase;
	hook_ymodem_file_info_t file;
	uint8_t packet_header;
	uint8_t packet_seq;
	uint8_t expected_seq;
	uint8_t error_count;
	uint8_t can_count;
	uint8_t eot_count;
	uint16_t packet_size;
	uint16_t data_index;
	uint16_t received_crc;
	uint16_t timeout_data_index;
	uint32_t last_progress_ms;
	hook_ymodem_rx_state_t timeout_state;
	uint8_t data[HOOK_YMODEM_PACKET_DATA_MAX];
} hook_ymodem_context_t;

static hook_ymodem_context_t hook_ymodem_ctx;

static uint8_t ymodem_max_errors(void)
{
	return (hook_ymodem_ctx.config.max_errors == 0U) ?
		       YMODEM_DEFAULT_MAX_ERRORS :
		       hook_ymodem_ctx.config.max_errors;
}

static uint32_t ymodem_packet_timeout_ms(void)
{
	return (hook_ymodem_ctx.config.packet_timeout_ms == 0U) ?
		       YMODEM_DEFAULT_PACKET_TIMEOUT_MS :
		       hook_ymodem_ctx.config.packet_timeout_ms;
}

static void ymodem_reset_session(void)
{
	hook_ymodem_config_t config = hook_ymodem_ctx.config;

	(void)memset(&hook_ymodem_ctx, 0, sizeof(hook_ymodem_ctx));
	hook_ymodem_ctx.config = config;
	hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_IDLE;
	hook_ymodem_ctx.phase = YMODEM_PHASE_WAIT_FILE_INFO;
}

static hook_ymodem_status_t ymodem_send_bytes(const uint8_t *data, uint16_t len)
{
	if ((data == NULL) || (len == 0U)) {
		return HOOK_YMODEM_ERR_PARAM;
	}

	if (hook_ymodem_ctx.config.send == NULL) {
		LOG_ERR("send callback missing");
		return HOOK_YMODEM_ERR_SEND;
	}

	if (hook_ymodem_ctx.config.send(data, len, hook_ymodem_ctx.config.user) != 0) {
		LOG_ERR("send callback failed (len=%u)", len);
		return HOOK_YMODEM_ERR_SEND;
	}

	return HOOK_YMODEM_OK;
}

hook_ymodem_status_t hook_ymodem_send_control(uint8_t control)
{
	return ymodem_send_bytes(&control, 1U);
}

static hook_ymodem_status_t ymodem_send_abort(hook_ymodem_status_t reason,
					      uint8_t send_cancel)
{
	static const uint8_t cancel[2] = {HOOK_YMODEM_CAN, HOOK_YMODEM_CAN};

	LOG_ERR("abort: reason=%d send_cancel=%u state=%u errors=%u", (int)reason,
		send_cancel, (unsigned int)hook_ymodem_ctx.rx_state,
		hook_ymodem_ctx.error_count);

	if (send_cancel != 0U) {
		(void)ymodem_send_bytes(cancel, (uint16_t)sizeof(cancel));
	}

	hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_ABORTED;
	hook_ymodem_ctx.phase = YMODEM_PHASE_ABORTED;

	if (hook_ymodem_ctx.config.on_abort != NULL) {
		hook_ymodem_ctx.config.on_abort(reason, hook_ymodem_ctx.config.user);
	}

	return reason;
}

static uint16_t get_ymodem_crc16(const uint8_t *data, uint16_t len)
{
	uint16_t crc = 0U;

	for (uint16_t i = 0U; i < len; ++i) {
		crc ^= (uint16_t)data[i] << 8U;

		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			if ((crc & 0x8000U) != 0U) {
				crc = (uint16_t)((crc << 1U) ^ 0x1021U);
			} else {
				crc = (uint16_t)(crc << 1U);
			}
		}
	}

	return crc;
}

static uint8_t ymodem_is_digit(uint8_t value)
{
	return (uint8_t)((value >= (uint8_t)'0') && (value <= (uint8_t)'9'));
}

static hook_ymodem_status_t ymodem_reject_packet(hook_ymodem_status_t reason)
{
	++hook_ymodem_ctx.error_count;
	LOG_ERR("reject packet: reason=%d count=%u/%u state=%u seq=%u expect=%u",
		(int)reason, hook_ymodem_ctx.error_count, ymodem_max_errors(),
		(unsigned int)hook_ymodem_ctx.rx_state,
		hook_ymodem_ctx.packet_seq, hook_ymodem_ctx.expected_seq);

	if (hook_ymodem_ctx.error_count >= ymodem_max_errors()) {
		return ymodem_send_abort(reason, 1U);
	}

	hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_FIND_HEADER;
	hook_ymodem_ctx.data_index = 0U;

	return hook_ymodem_send_control(HOOK_YMODEM_NAK);
}

static uint8_t ymodem_state_needs_timeout(hook_ymodem_rx_state_t state)
{
	return (uint8_t)((state == HOOK_YMODEM_RX_READ_PACKET_SEQ) ||
			 (state == HOOK_YMODEM_RX_READ_PACKET_SEQ_INV) ||
			 (state == HOOK_YMODEM_RX_RECV_FILE_INFO) ||
			 (state == HOOK_YMODEM_RX_RECV_FILE_DATA) ||
			 (state == HOOK_YMODEM_RX_RECV_FINISH_PACKET));
}

static hook_ymodem_status_t ymodem_accept_file_info_packet(void)
{
	static const uint8_t ack_and_crc_request[2] = {HOOK_YMODEM_ACK,
						      HOOK_YMODEM_CRC_REQUEST};
	uint16_t name_len = 0U;
	uint16_t copy_len;
	uint16_t pos;
	uint32_t size = 0U;
	uint8_t size_valid = 0U;

	if (hook_ymodem_ctx.packet_seq != 0U) {
		LOG_ERR("file info seq invalid: seq=%u", hook_ymodem_ctx.packet_seq);
		return ymodem_reject_packet(HOOK_YMODEM_ERR_SEQUENCE);
	}

	(void)memset(&hook_ymodem_ctx.file, 0, sizeof(hook_ymodem_ctx.file));

	if (hook_ymodem_ctx.data[0] == 0U) {
		/* Final (empty) block 0 after EOT: transfer complete. */
		hook_ymodem_ctx.phase = YMODEM_PHASE_DONE;
		hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_DONE;
		LOG_INF("empty file info packet, transfer done");
		return hook_ymodem_send_control(HOOK_YMODEM_ACK);
	}

	while ((name_len < hook_ymodem_ctx.packet_size) &&
	       (hook_ymodem_ctx.data[name_len] != 0U)) {
		++name_len;
	}

	if (name_len >= hook_ymodem_ctx.packet_size) {
		LOG_ERR("file info invalid: missing name terminator");
		return ymodem_reject_packet(HOOK_YMODEM_ERR_PACKET);
	}

	copy_len = name_len;
	if (copy_len >= HOOK_YMODEM_FILE_NAME_MAX) {
		copy_len = HOOK_YMODEM_FILE_NAME_MAX - 1U;
	}

	if (copy_len > 0U) {
		(void)memcpy(hook_ymodem_ctx.file.name, hook_ymodem_ctx.data, copy_len);
	}
	hook_ymodem_ctx.file.name[copy_len] = '\0';

	pos = (uint16_t)(name_len + 1U);
	while ((pos < hook_ymodem_ctx.packet_size) &&
	       (hook_ymodem_ctx.data[pos] == (uint8_t)' ')) {
		++pos;
	}

	while ((pos < hook_ymodem_ctx.packet_size) &&
	       (ymodem_is_digit(hook_ymodem_ctx.data[pos]) != 0U)) {
		uint32_t digit = (uint32_t)(hook_ymodem_ctx.data[pos] - (uint8_t)'0');

		if (size <= ((UINT32_MAX - digit) / 10U)) {
			size = (size * 10U) + digit;
		} else {
			size = UINT32_MAX;
		}

		size_valid = 1U;
		++pos;
	}

	hook_ymodem_ctx.file.size = size;
	hook_ymodem_ctx.file.size_valid = size_valid;
	hook_ymodem_ctx.file.bytes_received = 0U;

	LOG_INF("file info: name=%s size=%lu size_valid=%u",
		hook_ymodem_ctx.file.name, (unsigned long)hook_ymodem_ctx.file.size,
		hook_ymodem_ctx.file.size_valid);

	if (hook_ymodem_ctx.config.on_file_begin != NULL) {
		if (hook_ymodem_ctx.config.on_file_begin(&hook_ymodem_ctx.file,
							 hook_ymodem_ctx.config.user) != 0) {
			LOG_ERR("file begin callback failed");
			return ymodem_send_abort(HOOK_YMODEM_ERR_CALLBACK, 1U);
		}
	}

	hook_ymodem_ctx.expected_seq = 1U;
	hook_ymodem_ctx.phase = YMODEM_PHASE_WAIT_FILE_DATA;
	hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_FIND_HEADER;
	hook_ymodem_ctx.data_index = 0U;
	hook_ymodem_ctx.eot_count = 0U;

	return ymodem_send_bytes(ack_and_crc_request,
				 (uint16_t)sizeof(ack_and_crc_request));
}

static hook_ymodem_status_t ymodem_accept_data_packet(void)
{
	static const uint8_t ack_and_crc_request[2] = {HOOK_YMODEM_ACK,
						      HOOK_YMODEM_CRC_REQUEST};
	uint8_t previous_seq = (uint8_t)(hook_ymodem_ctx.expected_seq - 1U);
	uint32_t remaining;
	uint16_t payload_len;

	if (hook_ymodem_ctx.packet_seq == previous_seq) {
		/* Retransmission of the previous packet: ACK it again. */
		LOG_INF("duplicate packet ACK: seq=%u", hook_ymodem_ctx.packet_seq);
		hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_FIND_HEADER;
		hook_ymodem_ctx.data_index = 0U;

		if ((previous_seq == 0U) &&
		    (hook_ymodem_ctx.file.bytes_received == 0U)) {
			return ymodem_send_bytes(ack_and_crc_request,
						 (uint16_t)sizeof(ack_and_crc_request));
		}

		return hook_ymodem_send_control(HOOK_YMODEM_ACK);
	}

	if (hook_ymodem_ctx.packet_seq != hook_ymodem_ctx.expected_seq) {
		LOG_ERR("data seq invalid: seq=%u expect=%u",
			hook_ymodem_ctx.packet_seq, hook_ymodem_ctx.expected_seq);
		return ymodem_reject_packet(HOOK_YMODEM_ERR_SEQUENCE);
	}

	if (hook_ymodem_ctx.file.size_valid == 0U) {
		payload_len = hook_ymodem_ctx.packet_size;
	} else if (hook_ymodem_ctx.file.bytes_received >= hook_ymodem_ctx.file.size) {
		payload_len = 0U;
	} else {
		remaining = hook_ymodem_ctx.file.size -
			    hook_ymodem_ctx.file.bytes_received;
		payload_len = (remaining > (uint32_t)hook_ymodem_ctx.packet_size) ?
				      hook_ymodem_ctx.packet_size :
				      (uint16_t)remaining;
	}

	if ((payload_len > 0U) && (hook_ymodem_ctx.config.on_file_data != NULL)) {
		if (hook_ymodem_ctx.config.on_file_data(hook_ymodem_ctx.data, payload_len,
						       hook_ymodem_ctx.file.bytes_received,
						       hook_ymodem_ctx.config.user) != 0) {
			LOG_ERR("file data callback failed: offset=%lu len=%u",
				(unsigned long)hook_ymodem_ctx.file.bytes_received,
				payload_len);
			return ymodem_send_abort(HOOK_YMODEM_ERR_CALLBACK, 1U);
		}
	}

	hook_ymodem_ctx.file.bytes_received += payload_len;
	hook_ymodem_ctx.expected_seq = (uint8_t)(hook_ymodem_ctx.expected_seq + 1U);
	hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_FIND_HEADER;
	hook_ymodem_ctx.data_index = 0U;

	return hook_ymodem_send_control(HOOK_YMODEM_ACK);
}

static hook_ymodem_status_t ymodem_accept_finish_packet(void)
{
	if (hook_ymodem_ctx.packet_seq != 0U) {
		LOG_ERR("finish packet seq invalid: seq=%u", hook_ymodem_ctx.packet_seq);
		return ymodem_reject_packet(HOOK_YMODEM_ERR_SEQUENCE);
	}

	if (hook_ymodem_ctx.data[0] != 0U) {
		LOG_ERR("finish packet invalid: first_byte=0x%02x", hook_ymodem_ctx.data[0]);
		return ymodem_reject_packet(HOOK_YMODEM_ERR_PACKET);
	}

	hook_ymodem_ctx.phase = YMODEM_PHASE_DONE;
	hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_DONE;

	if (hook_ymodem_send_control(HOOK_YMODEM_ACK) != HOOK_YMODEM_OK) {
		LOG_ERR("finish packet ACK send failed");
		return HOOK_YMODEM_ERR_SEND;
	}

	if (hook_ymodem_ctx.config.on_file_end != NULL) {
		LOG_INF("finish packet accepted, call file end");
		hook_ymodem_ctx.config.on_file_end(&hook_ymodem_ctx.file,
						   hook_ymodem_ctx.config.user);
	}

	return HOOK_YMODEM_OK;
}

static hook_ymodem_status_t ymodem_finish_packet(
	hook_ymodem_status_t (*accept_packet)(void))
{
	uint16_t calc_crc = get_ymodem_crc16(hook_ymodem_ctx.data,
					     hook_ymodem_ctx.packet_size);

	if (calc_crc != hook_ymodem_ctx.received_crc) {
		LOG_ERR("CRC mismatch: seq=%u calc=0x%04x recv=0x%04x",
			hook_ymodem_ctx.packet_seq, calc_crc,
			hook_ymodem_ctx.received_crc);
		return ymodem_reject_packet(HOOK_YMODEM_ERR_CRC);
	}

	hook_ymodem_ctx.error_count = 0U;
	hook_ymodem_ctx.can_count = 0U;

	if (accept_packet == NULL) {
		return ymodem_reject_packet(HOOK_YMODEM_ERR_PACKET);
	}

	return accept_packet();
}

static hook_ymodem_status_t ymodem_receive_packet_byte(
	uint8_t byte, hook_ymodem_status_t (*accept_packet)(void))
{
	if (hook_ymodem_ctx.data_index < hook_ymodem_ctx.packet_size) {
		hook_ymodem_ctx.data[hook_ymodem_ctx.data_index] = byte;
		++hook_ymodem_ctx.data_index;
		return HOOK_YMODEM_OK;
	}

	if (hook_ymodem_ctx.data_index == hook_ymodem_ctx.packet_size) {
		hook_ymodem_ctx.received_crc = (uint16_t)byte << 8U;
		++hook_ymodem_ctx.data_index;
		return HOOK_YMODEM_OK;
	}

	hook_ymodem_ctx.received_crc |= byte;

	return ymodem_finish_packet(accept_packet);
}

static hook_ymodem_status_t ymodem_handle_eot(void)
{
	static const uint8_t ack_and_crc_request[2] = {HOOK_YMODEM_ACK,
						      HOOK_YMODEM_CRC_REQUEST};

	if (hook_ymodem_ctx.phase != YMODEM_PHASE_WAIT_FILE_DATA) {
		LOG_ERR("EOT in invalid phase=%u", (unsigned int)hook_ymodem_ctx.phase);
		return ymodem_reject_packet(HOOK_YMODEM_ERR_PACKET);
	}

	if (hook_ymodem_ctx.eot_count == 0U) {
		hook_ymodem_ctx.eot_count = 1U;
		LOG_INF("first EOT received, send NAK");
		return hook_ymodem_send_control(HOOK_YMODEM_NAK);
	}

	hook_ymodem_ctx.eot_count = 0U;
	hook_ymodem_ctx.phase = YMODEM_PHASE_WAIT_FINISH_PACKET;
	hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_FIND_HEADER;

	LOG_INF("second EOT received, wait finish packet");

	return ymodem_send_bytes(ack_and_crc_request,
				 (uint16_t)sizeof(ack_and_crc_request));
}

void hook_ymodem_init(const hook_ymodem_config_t *config)
{
	(void)memset(&hook_ymodem_ctx, 0, sizeof(hook_ymodem_ctx));

	if (config != NULL) {
		hook_ymodem_ctx.config = *config;
	}

	hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_IDLE;
	hook_ymodem_ctx.phase = YMODEM_PHASE_WAIT_FILE_INFO;
}

hook_ymodem_status_t hook_ymodem_start_receive(void)
{
	ymodem_reset_session();

	if (hook_ymodem_ctx.config.send == NULL) {
		LOG_ERR("start receive failed: send callback missing");
		return HOOK_YMODEM_ERR_SEND;
	}

	hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_FIND_HEADER;
	hook_ymodem_ctx.phase = YMODEM_PHASE_WAIT_FILE_INFO;
	hook_ymodem_ctx.expected_seq = 0U;

	return hook_ymodem_send_control(HOOK_YMODEM_CRC_REQUEST);
}

void hook_ymodem_reset(void)
{
	ymodem_reset_session();
}

hook_ymodem_status_t hook_ymodem_input_byte(uint8_t byte)
{
	if ((hook_ymodem_ctx.rx_state == HOOK_YMODEM_RX_IDLE) ||
	    (hook_ymodem_ctx.rx_state == HOOK_YMODEM_RX_DONE) ||
	    (hook_ymodem_ctx.rx_state == HOOK_YMODEM_RX_ABORTED)) {
		return HOOK_YMODEM_OK;
	}

	switch (hook_ymodem_ctx.rx_state) {
	case HOOK_YMODEM_RX_FIND_HEADER:
		if (byte == HOOK_YMODEM_CAN) {
			++hook_ymodem_ctx.can_count;
			if (hook_ymodem_ctx.can_count >= YMODEM_CAN_COUNT_TO_ABORT) {
				LOG_ERR("host cancelled transfer");
				return ymodem_send_abort(HOOK_YMODEM_ERR_ABORTED, 0U);
			}
			return HOOK_YMODEM_OK;
		}

		hook_ymodem_ctx.can_count = 0U;

		if ((byte == HOOK_YMODEM_SOH) || (byte == HOOK_YMODEM_STX)) {
			hook_ymodem_ctx.packet_header = byte;
			hook_ymodem_ctx.packet_size =
				(byte == HOOK_YMODEM_SOH) ? YMODEM_SOH_DATA_SIZE :
							    YMODEM_STX_DATA_SIZE;
			hook_ymodem_ctx.packet_seq = 0U;
			hook_ymodem_ctx.data_index = 0U;
			hook_ymodem_ctx.received_crc = 0U;
			hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_READ_PACKET_SEQ;
		} else if (byte == HOOK_YMODEM_EOT) {
			return ymodem_handle_eot();
		} else {
			/* Ignore noise until a frame header is found. */
		}
		break;

	case HOOK_YMODEM_RX_READ_PACKET_SEQ:
		hook_ymodem_ctx.packet_seq = byte;
		hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_READ_PACKET_SEQ_INV;
		break;

	case HOOK_YMODEM_RX_READ_PACKET_SEQ_INV:
		if ((uint8_t)(hook_ymodem_ctx.packet_seq + byte) != 0xFFU) {
			hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_FIND_HEADER;
			hook_ymodem_ctx.data_index = 0U;
			++hook_ymodem_ctx.error_count;
			LOG_ERR("seq inverse invalid: seq=%u inv=%u count=%u/%u",
				hook_ymodem_ctx.packet_seq, byte,
				hook_ymodem_ctx.error_count, ymodem_max_errors());

			if (hook_ymodem_ctx.error_count >= ymodem_max_errors()) {
				return ymodem_send_abort(HOOK_YMODEM_ERR_SEQUENCE, 1U);
			}

			return hook_ymodem_send_control(HOOK_YMODEM_NAK);
		}

		if (hook_ymodem_ctx.phase == YMODEM_PHASE_WAIT_FILE_INFO) {
			hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_RECV_FILE_INFO;
		} else if (hook_ymodem_ctx.phase == YMODEM_PHASE_WAIT_FILE_DATA) {
			hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_RECV_FILE_DATA;
		} else if (hook_ymodem_ctx.phase == YMODEM_PHASE_WAIT_FINISH_PACKET) {
			hook_ymodem_ctx.rx_state = HOOK_YMODEM_RX_RECV_FINISH_PACKET;
		} else {
			return ymodem_reject_packet(HOOK_YMODEM_ERR_PACKET);
		}

		hook_ymodem_ctx.data_index = 0U;
		break;

	case HOOK_YMODEM_RX_RECV_FILE_INFO:
		return ymodem_receive_packet_byte(byte, ymodem_accept_file_info_packet);

	case HOOK_YMODEM_RX_RECV_FILE_DATA:
		return ymodem_receive_packet_byte(byte, ymodem_accept_data_packet);

	case HOOK_YMODEM_RX_RECV_FINISH_PACKET:
		return ymodem_receive_packet_byte(byte, ymodem_accept_finish_packet);

	case HOOK_YMODEM_RX_IDLE:
	case HOOK_YMODEM_RX_DONE:
	case HOOK_YMODEM_RX_ABORTED:
	default:
		break;
	}

	return HOOK_YMODEM_OK;
}

hook_ymodem_status_t hook_ymodem_tick(uint32_t now_ms)
{
	if (ymodem_state_needs_timeout(hook_ymodem_ctx.rx_state) == 0U) {
		hook_ymodem_ctx.timeout_state = hook_ymodem_ctx.rx_state;
		hook_ymodem_ctx.timeout_data_index = hook_ymodem_ctx.data_index;
		hook_ymodem_ctx.last_progress_ms = now_ms;
		return HOOK_YMODEM_OK;
	}

	if ((hook_ymodem_ctx.timeout_state != hook_ymodem_ctx.rx_state) ||
	    (hook_ymodem_ctx.timeout_data_index != hook_ymodem_ctx.data_index)) {
		hook_ymodem_ctx.timeout_state = hook_ymodem_ctx.rx_state;
		hook_ymodem_ctx.timeout_data_index = hook_ymodem_ctx.data_index;
		hook_ymodem_ctx.last_progress_ms = now_ms;
		return HOOK_YMODEM_OK;
	}

	if ((uint32_t)(now_ms - hook_ymodem_ctx.last_progress_ms) <
	    ymodem_packet_timeout_ms()) {
		return HOOK_YMODEM_OK;
	}

	LOG_ERR("packet timeout: state=%u data_index=%u timeout=%lu",
		(unsigned int)hook_ymodem_ctx.rx_state, hook_ymodem_ctx.data_index,
		(unsigned long)ymodem_packet_timeout_ms());

	return ymodem_reject_packet(HOOK_YMODEM_ERR_TIMEOUT);
}

hook_ymodem_rx_state_t hook_ymodem_get_state(void)
{
	return hook_ymodem_ctx.rx_state;
}

const hook_ymodem_file_info_t *hook_ymodem_get_file_info(void)
{
	return &hook_ymodem_ctx.file;
}

bool hook_ymodem_is_busy(void)
{
	return (hook_ymodem_ctx.rx_state != HOOK_YMODEM_RX_IDLE) &&
	       (hook_ymodem_ctx.rx_state != HOOK_YMODEM_RX_DONE) &&
	       (hook_ymodem_ctx.rx_state != HOOK_YMODEM_RX_ABORTED);
}

bool hook_ymodem_is_done(void)
{
	return hook_ymodem_ctx.rx_state == HOOK_YMODEM_RX_DONE;
}

bool hook_ymodem_waiting_for_packet(void)
{
	return (hook_ymodem_ctx.rx_state == HOOK_YMODEM_RX_FIND_HEADER) ||
	       (hook_ymodem_ctx.rx_state == HOOK_YMODEM_RX_READ_PACKET_SEQ) ||
	       (hook_ymodem_ctx.rx_state == HOOK_YMODEM_RX_READ_PACKET_SEQ_INV);
}
