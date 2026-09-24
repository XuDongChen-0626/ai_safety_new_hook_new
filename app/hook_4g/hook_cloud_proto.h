/*
 * hook_cloud_proto.h - binary cloud protocol (0xA5 frames).
 *
 * Ported from application/app_4g.c. Frame layout on the wire, as delivered
 * by the usr.cn module:
 *
 *   host -> device : "1," <0xA5> <content id> ... <crc lo> <crc hi>   (AT)
 *                    "3," <0xA5> <seq> <id> <len> <data...> <crc16>
 *   device -> host : "3," <0xA5> <seq> <id> <len> <data...> <crc16>
 *
 * CRC is CRC16-CCITT (poly 0x1021, init 0x0000), transmitted low byte first.
 */
#ifndef HOOK_CLOUD_PROTO_H
#define HOOK_CLOUD_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOOK_CLOUD_FRAME_MAGIC 0xA5U

/* Command IDs (cloud -> device). */
#define CMD_ID_LIGHT         0x01U /* LED/sound control            */
#define CMD_ID_BMS           0x02U /* battery information          */
#define CMD_ID_HEARTBEAT_EN  0x04U /* heartbeat supervision enable */
#define CMD_ID_HEARTBEAT     0x05U /* host heartbeat counter       */
#define CMD_ID_WORK_MODE     0x06U /* work mode (read only)        */
#define CMD_ID_SLEEP_EN      0x07U /* soft standby enable          */
#define CMD_ID_SLEEP_TIME    0x08U /* power on/off schedule        */
#define CMD_ID_CURRENT_TIME  0x09U /* RTC date/time (calibration)  */
#define CMD_ID_SYS_ERROR     0xF0U /* system error code            */

/* Reply IDs (device -> cloud), request id | 0x80 on error. */
#define ACK_ID_LIGHT        0x81U
#define ACK_ID_BMS          0x82U
#define ACK_ID_SYSTEM_STATUS 0x83U
#define ACK_ID_TIME_SCHEDULE 0x84U
#define ACK_ID_HEARTBEAT    0x85U

/* Error codes carried in the reply payload. */
#define HOOK_CLOUD_ERR_CRC_FAIL   0x01U
#define HOOK_CLOUD_ERR_LEN        0x02U
#define HOOK_CLOUD_ERR_UNKNOWN_ID 0x03U
#define HOOK_CLOUD_ERR_R_W        0x04U

void hook_cloud_proto_init(void);
void hook_cloud_proto_feed(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_CLOUD_PROTO_H */
