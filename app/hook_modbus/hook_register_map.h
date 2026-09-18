/*
 * hook_register_map.h - hook Modbus register map.
 *
 * The map is split by direction, not by "importance":
 *
 *   holding registers 0..255 (FC03 / FC06 / FC16)
 *       everything the host / cloud WRITES: commands, configuration and the
 *       one shot RTC calibration command.
 *   input registers   0..255 (FC04)
 *       everything the host / cloud READS: device status, BMS telemetry and
 *       the current time. The firmware updates them, the host only reads
 *       them, so a status value can never be overwritten from outside.
 *
 * The controller answers as a Modbus RTU slave on the host RS485 bus (uart8)
 * and publishes the same values to the cloud through the 4G / 0xA5 protocol,
 * so moving a value between the two tables only changes the firmware mapping
 * (and the bench Modbus master); the cloud protocol stays identical.
 */
#ifndef HOOK_REGISTER_MAP_H
#define HOOK_REGISTER_MAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Holding registers - written by the host / cloud                     */
/* ================================================================== */

/* --- commands (0x00..) --- */
#define CMD_LED_SWITCH                   0x00U /* flash light flashing on    */
#define CMD_BUZZER_7M                    0x01U /* 7 m alarm sound trigger    */
#define CMD_BUZZER_3M                    0x02U /* 3 m alarm sound trigger    */
#define CMD_VOLUME                       0x03U /* alarm module volume 0..31  */
#define CMD_HOST_HEARTBEAT               0x04U /* host heartbeat counter: the
						* host increments it, a change
						* is the "host is alive" signal
						*/

/* --- configuration (0x10..) --- */
#define POWER_ON_TIME                    0x10U /* daily power-on, BJ hh:mm   */
#define POWER_OFF_TIME                   0x11U /* daily power-off, BJ hh:mm  */
#define STM32_STANDBY_ENABLE             0x12U /* 1 = STM32 STANDBY (alarm B)*/
#define SOFT_STANDBY_ENABLE              0x13U /* 1 = cut outputs in window  */
#define HEARTBEAT_ENABLE                 0x14U /* 1 = supervise the host     */

/* --- RTC calibration command, one shot (0x20..) --- */
#define RTC_CAL_YEAR                     0x20U
#define RTC_CAL_MONTH_DAY                0x21U /* (month << 8) | day         */
#define RTC_CAL_HHMM                     0x22U /* (hour << 8) | minute       */

/* "not written yet" markers of the one shot calibration command. 0 is never
 * a valid year/month-day; 00:00 is a valid time, so hh:mm uses 0xFFFF. A
 * partial command is discarded after RTC_CAL_TIMEOUT_MS (hook_rtc.c).
 */
#define RTC_CAL_YEAR_DEFAULT             0U
#define RTC_CAL_MONTH_DAY_DEFAULT        0U
#define RTC_CAL_HHMM_DEFAULT             0xFFFFU

/* ================================================================== */
/* Input registers - published by the firmware, read by the host /     */
/* cloud (FC04). Written with hook_reg_input_set().                    */
/* ================================================================== */

/* --- device status (0x00..) --- */
#define STATUS_WORK_MODE                 0x00U /* 0 standby 1 normal 2 low
						* battery 3 error
						*/
#define REG_ERROR_CODE                   0x01U /* error bits, see system_def */
#define STATUS_LED_SWITCH                0x02U /* 1 = flash light blinking   */
#define STATUS_BUZZER                    0x03U /* 0 off 1 7 m 2 3 m          */
#define STATUS_BMS_BATTERY               0x04U
#define STATUS_BMS_TOTAL_VOLTAGE         0x05U
#define STATUS_BMS_TOTAL_CURRENT         0x06U
#define STATUS_BMS_IS_CHARGE             0x07U /* 0 discharge 1 charge 2 idle*/
#define STATUS_BMS_REMAIN_DISCHARGE_TIME 0x08U
#define STATUS_BMS_REMAIN_CHARGE_TIME    0x09U
#define STATUS_BMS_PROTECT_STATUS        0x0AU

/* --- current time display (0x10..) --- */
#define INPUT_RTC_YEAR                   0x10U
#define INPUT_RTC_MONTH_DAY              0x11U /* (month << 8) | day         */
#define INPUT_RTC_TIME_HHMM              0x12U /* (hour << 8) | minute       */

#ifdef __cplusplus
}
#endif

#endif /* HOOK_REGISTER_MAP_H */
