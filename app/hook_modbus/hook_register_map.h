/*
 * hook_register_map.h - hook Modbus register map.
 *
 * The hook controller is a Modbus RTU slave on the host RS485 bus. The
 * addresses below are identical to the original firmware
 * (application/system_def.h) so that existing hosts keep working:
 *
 *   holding registers : 0 .. 255   (FC03 / FC06 / FC16)
 *   input registers   : 0 .. 255   (FC04, reserved)
 *
 * Register 0..99  : host commands / reserved
 * Register 100..118: device status and configuration
 */
#ifndef HOOK_REGISTER_MAP_H
#define HOOK_REGISTER_MAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Host commands (written by the host / cloud)                         */
/* ------------------------------------------------------------------ */
#define CMD_LED_SWITCH   0U   /* LED on/off request (1 = flashing on)  */
#define CMD_BUZZER_7M    1U   /* 7 m alarm sound trigger               */
#define CMD_BUZZER_3M    2U   /* 3 m alarm sound trigger               */

#define CMD_VOLUME       103U /* [rw] alarm module volume (0..31)      */

/* ------------------------------------------------------------------ */
/* Device status (read by the host / cloud)                            */
/* ------------------------------------------------------------------ */
#define STATUS_LED_SWITCH                100U
#define STATUS_BUZZER                    101U
#define STATUS_BMS_BATTERY               102U
#define STATUS_HEART_BEAT                104U /* host heartbeat counter  */
#define STATUS_BMS_REMAIN_DISCHARGE_TIME 105U
#define STATUS_WORK_MODE                 106U
#define STATUS_BMS_IS_charge             107U /* 0 discharge 1 charge 2 idle */
#define STATUS_BMS_REMAIN_CHARGE_TIME    108U
#define STATUS_BMS_TOTAL_VOLTAGE         109U
#define STATUS_BMS_TOTAL_CURRENT         110U
#define STATUS_POWER_OFF_TIME            111U
#define STATUS_POWER_ON_TIME             112U
#define REG_ERROR_CODE                   113U
#define RTC_TIME                         114U
#define STM32_STANDBY_ENABLE             115U /* hard standby (RTC alarm B) */
#define SOFT_STANDBY_ENABLE              116U /* soft standby (relay off)   */
#define HEARTBEAT_ENABLE                 117U
#define STATUS_BMS_PROTECT_STATUS        118U

#ifdef __cplusplus
}
#endif

#endif /* HOOK_REGISTER_MAP_H */
