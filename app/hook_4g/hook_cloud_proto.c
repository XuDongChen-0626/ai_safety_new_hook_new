/*
 * hook_cloud_proto.c - binary cloud protocol (0xA5 frames).
 */
#include "hook_cloud_proto.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "hook_register.h"
#include "hook_register_map.h"
#include "system_def.h"
#include "uart_manage.h"

LOG_MODULE_REGISTER(hook_cloud, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_CLOUD_DATA_MAX 256U

typedef enum {
	STATE_WAIT_START = 0,
	STATE_READ_HEADER,
	STATE_READ_DATA,
	STATE_READ_CRC,
} hook_cloud_state_t;

typedef struct {
	hook_cloud_state_t state;
	uint8_t header_buf[4];
	uint8_t header_cnt;
	uint8_t data_buf[HOOK_CLOUD_DATA_MAX];
	uint16_t data_len;
	uint16_t data_cnt;
	uint8_t crc_buf[2];
	uint8_t crc_cnt;
} hook_cloud_parser_t;

static hook_cloud_parser_t hook_cloud_parser;
static uint8_t hook_cloud_tx_seq;

static uint16_t hook_cloud_crc16_ccitt(const uint8_t *buf, uint32_t len)
{
	uint16_t crc = 0U;

	for (uint32_t i = 0U; i < len; i++) {
		crc ^= (uint16_t)((uint16_t)buf[i] << 8);
		for (uint8_t bit = 0U; bit < 8U; bit++) {
			if ((crc & 0x8000U) != 0U) {
				crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021U);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
	}

	return crc;
}

static void hook_cloud_send_ack(uint8_t ack_id, const uint8_t *payload,
				uint8_t payload_len)
{
	uint8_t tx_buf[8U + HOOK_CLOUD_DATA_MAX];
	uint16_t idx = 0U;
	uint16_t frame_start;
	uint16_t crc;

	/* Everything the device reports to the cloud uses channel 3. */
	tx_buf[idx++] = '3';
	tx_buf[idx++] = ',';

	frame_start = idx;
	tx_buf[idx++] = HOOK_CLOUD_FRAME_MAGIC;
	tx_buf[idx++] = hook_cloud_tx_seq++;
	tx_buf[idx++] = ack_id;
	tx_buf[idx++] = payload_len;

	if ((payload_len > 0U) && (payload != NULL)) {
		memcpy(&tx_buf[idx], payload, payload_len);
		idx += payload_len;
	}

	crc = hook_cloud_crc16_ccitt(&tx_buf[frame_start], idx - frame_start);
	tx_buf[idx++] = (uint8_t)(crc & 0xFFU);
	tx_buf[idx++] = (uint8_t)((crc >> 8) & 0xFFU);

	(void)uart_manage_send_by_name("4g", tx_buf, idx);
}

static void hook_cloud_put_u16(uint8_t *dst, uint16_t value)
{
	dst[0] = (uint8_t)(value & 0xFFU);
	dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/* --- cloud -> device commands ------------------------------------- */

static void hook_cloud_handle_light(uint8_t content_id, const uint8_t *data,
				    uint8_t data_len)
{
	if (data_len == 0U) {
		uint8_t status = 0U;
		uint8_t volume = (uint8_t)(hook_reg_get(CMD_VOLUME) & 0x1FU);

		if (hook_reg_get(STATUS_LED_SWITCH) != 0U) {
			status |= (1U << 0);
		}
		if (hook_reg_get(STATUS_BUZZER) == 1U) {
			status |= (1U << 1);
		} else if (hook_reg_get(STATUS_BUZZER) == 2U) {
			status |= (1U << 2);
		}
		status |= (uint8_t)(volume << 3);

		hook_cloud_send_ack(content_id, &status, 1U);
		return;
	}

	hook_reg_set(CMD_LED_SWITCH, (data[0] & 0x01U) ? 1U : 0U);
	hook_reg_set(CMD_BUZZER_7M, (data[0] & 0x02U) ? 1U : 0U);
	hook_reg_set(CMD_BUZZER_3M, (data[0] & 0x04U) ? 1U : 0U);
	hook_reg_set(CMD_VOLUME, (uint16_t)((data[0] >> 3) & 0x1FU));

	hook_cloud_send_ack(content_id, NULL, 0U);
}

static void hook_cloud_handle_bms(uint8_t content_id, const uint8_t *data,
				  uint8_t data_len)
{
	if (data_len != 0U) {
		uint8_t err = HOOK_CLOUD_ERR_R_W;

		hook_cloud_send_ack((uint8_t)(content_id | 0x80U), &err, 1U);
		return;
	}

	{
		uint8_t payload[12];

		hook_cloud_put_u16(&payload[0], hook_reg_get(STATUS_BMS_BATTERY));
		hook_cloud_put_u16(&payload[2], hook_reg_get(STATUS_BMS_TOTAL_VOLTAGE));
		hook_cloud_put_u16(&payload[4], hook_reg_get(STATUS_BMS_TOTAL_CURRENT));
		hook_cloud_put_u16(&payload[6],
				   hook_reg_get(STATUS_BMS_REMAIN_DISCHARGE_TIME));
		hook_cloud_put_u16(&payload[8],
				   hook_reg_get(STATUS_BMS_REMAIN_CHARGE_TIME));
		hook_cloud_put_u16(&payload[10],
				   hook_reg_get(STATUS_BMS_PROTECT_STATUS));

		hook_cloud_send_ack(content_id, payload, sizeof(payload));
	}
}

static void hook_cloud_handle_heartbeat_en(uint8_t content_id,
					   const uint8_t *data,
					   uint8_t data_len)
{
	if (data_len == 0U) {
		uint8_t enabled = (uint8_t)(hook_reg_get(HEARTBEAT_ENABLE) & 0xFFU);

		hook_cloud_send_ack(content_id, &enabled, 1U);
		return;
	}

	hook_reg_set(HEARTBEAT_ENABLE, data[0]);
	hook_cloud_send_ack(content_id, NULL, 0U);
}

static void hook_cloud_handle_heartbeat(uint8_t content_id, const uint8_t *data,
					uint8_t data_len)
{
	if (data_len == 0U) {
		uint8_t heartbeat =
			(uint8_t)(hook_reg_get(STATUS_HEART_BEAT) & 0xFFU);

		hook_cloud_send_ack(content_id, &heartbeat, 1U);
		return;
	}

	/* Heartbeat writes are not acknowledged (same as the old firmware). */
	hook_reg_set(STATUS_HEART_BEAT, data[0]);
}

static void hook_cloud_handle_work_mode(uint8_t content_id,
					const uint8_t *data, uint8_t data_len)
{
	if (data_len == 0U) {
		uint8_t payload[2] = {0U, 0U};
		uint16_t mode = hook_reg_get(STATUS_WORK_MODE);

		payload[0] = (uint8_t)(mode & 0xFFU);

		/* In DEVICE_ERROR the system error code is appended in byte 1,
		 * otherwise byte 1 is zero (latest firmware behaviour).
		 */
		if (mode == (uint16_t)DEVICE_ERROR) {
			payload[1] = (uint8_t)(hook_reg_get(REG_ERROR_CODE) & 0xFFU);
		}

		hook_cloud_send_ack(content_id, payload, sizeof(payload));
	} else {
		/* Work mode is read-only. */
		uint8_t err = HOOK_CLOUD_ERR_R_W;

		hook_cloud_send_ack((uint8_t)(content_id | 0x80U), &err, 1U);
	}
}

static void hook_cloud_handle_sleep_en(uint8_t content_id,
				       const uint8_t *data, uint8_t data_len)
{
	if (data_len == 0U) {
		uint8_t enabled = (uint8_t)(hook_reg_get(SOFT_STANDBY_ENABLE) & 0xFFU);

		hook_cloud_send_ack(content_id, &enabled, 1U);
		return;
	}

	hook_reg_set(SOFT_STANDBY_ENABLE, data[0]);
	hook_cloud_send_ack(content_id, NULL, 0U);
}

static void hook_cloud_handle_sleep_time(uint8_t content_id,
					 const uint8_t *data, uint8_t data_len)
{
	if (data_len == 0U) {
		uint8_t payload[4];

		hook_cloud_put_u16(&payload[0], hook_reg_get(STATUS_POWER_ON_TIME));
		hook_cloud_put_u16(&payload[2], hook_reg_get(STATUS_POWER_OFF_TIME));

		hook_cloud_send_ack(content_id, payload, sizeof(payload));
		return;
	}

	if (data_len >= 4U) {
		uint16_t on_time = (uint16_t)(data[0] | (data[1] << 8));
		uint16_t off_time = (uint16_t)(data[2] | (data[3] << 8));

		hook_reg_set(STATUS_POWER_ON_TIME, on_time);
		hook_reg_set(STATUS_POWER_OFF_TIME, off_time);
		hook_cloud_send_ack(content_id, NULL, 0U);
	}
}

static void hook_cloud_handle_current_time(uint8_t content_id,
					   const uint8_t *data,
					   uint8_t data_len)
{
	if (data_len == 0U) {
		uint8_t payload[2];

		hook_cloud_put_u16(payload, hook_reg_get(RTC_TIME));
		hook_cloud_send_ack(content_id, payload, sizeof(payload));
		return;
	}

	if (data_len >= 2U) {
		hook_reg_set(RTC_TIME, (uint16_t)(data[0] | (data[1] << 8)));
		hook_cloud_send_ack(content_id, NULL, 0U);
	}
}

static void hook_cloud_handle_sys_error(uint8_t content_id,
					const uint8_t *data, uint8_t data_len)
{
	if (data_len == 0U) {
		uint8_t err = (uint8_t)(hook_reg_get(REG_ERROR_CODE) & 0xFFU);

		hook_cloud_send_ack(content_id, &err, 1U);
	}
}

typedef struct {
	uint8_t content_id;
	void (*handler)(uint8_t content_id, const uint8_t *data, uint8_t data_len);
} hook_cloud_handler_t;

static const hook_cloud_handler_t hook_cloud_handlers[] = {
	{CMD_ID_LIGHT, hook_cloud_handle_light},
	{CMD_ID_BMS, hook_cloud_handle_bms},
	{CMD_ID_HEARTBEAT_EN, hook_cloud_handle_heartbeat_en},
	{CMD_ID_HEARTBEAT, hook_cloud_handle_heartbeat},
	{CMD_ID_WORK_MODE, hook_cloud_handle_work_mode},
	{CMD_ID_SLEEP_EN, hook_cloud_handle_sleep_en},
	{CMD_ID_SLEEP_TIME, hook_cloud_handle_sleep_time},
	{CMD_ID_CURRENT_TIME, hook_cloud_handle_current_time},
	{CMD_ID_SYS_ERROR, hook_cloud_handle_sys_error},
};

static void hook_cloud_frame_complete(void)
{
	uint8_t content_id = hook_cloud_parser.header_buf[2];
	uint8_t data_len = hook_cloud_parser.header_buf[3];

	for (size_t i = 0U; i < ARRAY_SIZE(hook_cloud_handlers); i++) {
		if (hook_cloud_handlers[i].content_id == content_id) {
			hook_cloud_handlers[i].handler(content_id,
						       hook_cloud_parser.data_buf,
						       data_len);
			return;
		}
	}

	{
		uint8_t err = HOOK_CLOUD_ERR_UNKNOWN_ID;

		LOG_WRN("unknown cloud content id 0x%02X", content_id);
		hook_cloud_send_ack((uint8_t)(content_id | 0x80U), &err, 1U);
	}
}

void hook_cloud_proto_init(void)
{
	memset(&hook_cloud_parser, 0, sizeof(hook_cloud_parser));
	hook_cloud_parser.state = STATE_WAIT_START;
	hook_cloud_tx_seq = 0U;
}

void hook_cloud_proto_feed(const uint8_t *buf, uint16_t len)
{
	if (buf == NULL) {
		return;
	}

	for (uint16_t i = 0U; i < len; i++) {
		uint8_t byte = buf[i];

		switch (hook_cloud_parser.state) {
		case STATE_WAIT_START:
			if (byte == HOOK_CLOUD_FRAME_MAGIC) {
				hook_cloud_parser.header_buf[0] = byte;
				hook_cloud_parser.header_cnt = 1U;
				hook_cloud_parser.state = STATE_READ_HEADER;
			}
			break;

		case STATE_READ_HEADER:
			hook_cloud_parser.header_buf[hook_cloud_parser.header_cnt++] = byte;
			if (hook_cloud_parser.header_cnt == 4U) {
				hook_cloud_parser.data_len = hook_cloud_parser.header_buf[3];
				if (hook_cloud_parser.data_len == 0U) {
					hook_cloud_parser.crc_cnt = 0U;
					hook_cloud_parser.state = STATE_READ_CRC;
				} else if (hook_cloud_parser.data_len > HOOK_CLOUD_DATA_MAX) {
					LOG_ERR("cloud frame too long (%u bytes)",
						hook_cloud_parser.data_len);
					hook_cloud_parser.state = STATE_WAIT_START;
				} else {
					hook_cloud_parser.data_cnt = 0U;
					hook_cloud_parser.state = STATE_READ_DATA;
				}
			}
			break;

		case STATE_READ_DATA:
			hook_cloud_parser.data_buf[hook_cloud_parser.data_cnt++] = byte;
			if (hook_cloud_parser.data_cnt == hook_cloud_parser.data_len) {
				hook_cloud_parser.crc_cnt = 0U;
				hook_cloud_parser.state = STATE_READ_CRC;
			}
			break;

		case STATE_READ_CRC: {
			uint16_t received_crc;
			uint16_t calculated_crc;
			uint8_t calc_buf[4U + HOOK_CLOUD_DATA_MAX];

			hook_cloud_parser.crc_buf[hook_cloud_parser.crc_cnt++] = byte;
			if (hook_cloud_parser.crc_cnt < 2U) {
				break;
			}

			received_crc = (uint16_t)(hook_cloud_parser.crc_buf[0] |
						  (hook_cloud_parser.crc_buf[1] << 8));

			memcpy(calc_buf, hook_cloud_parser.header_buf, 4U);
			if (hook_cloud_parser.data_len > 0U) {
				memcpy(&calc_buf[4], hook_cloud_parser.data_buf,
				       hook_cloud_parser.data_len);
			}
			calculated_crc = hook_cloud_crc16_ccitt(
				calc_buf, (uint32_t)(4U + hook_cloud_parser.data_len));

			if (calculated_crc == received_crc) {
				hook_cloud_frame_complete();
			} else {
				uint8_t req_id = hook_cloud_parser.header_buf[2];
				uint8_t err = HOOK_CLOUD_ERR_CRC_FAIL;

				LOG_WRN("cloud frame CRC error (calc 0x%04X rcv 0x%04X)",
					calculated_crc, received_crc);
				hook_cloud_send_ack((uint8_t)(req_id | 0x80U), &err, 1U);
			}

			hook_cloud_parser.state = STATE_WAIT_START;
			break;
		}
		}
	}
}
