/*
 * system_def.h - shared product definitions for the crane AI safety hook.
 *
 * Ported from the original STM32CubeIDE firmware (crane_ai_satefy_hook.ioc):
 * thresholds, timezone, work-mode/error enums and external device constants.
 * The Modbus register map itself lives in app/hook_modbus/hook_register_map.h
 * (registered as a data model table in app/hook_modbus/hook_register.c).
 *
 * Logging: the old printf-style LOGD/LOGI/LOGE macros are replaced by the
 * Zephyr logging subsystem. Every translation unit registers its own module
 * with LOG_MODULE_REGISTER().
 */
#ifndef HOOK_SYSTEM_DEF_H
#define HOOK_SYSTEM_DEF_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Scheduling / thresholds                                             */
/* ------------------------------------------------------------------ */
#define TIMEZONE_OFFSET_BEIJING 8       /* UTC+8 */

#ifdef CONFIG_HOOK_HEARTBEAT_SILENCE_MS
#define HEARTBEAT_SILENCE_MS CONFIG_HOOK_HEARTBEAT_SILENCE_MS
#else
#define HEARTBEAT_SILENCE_MS 10000
#endif

#ifdef CONFIG_HOOK_HEARTBEAT_TIMEOUT_MS
#define HEARTBEAT_TIMEOUT_MS CONFIG_HOOK_HEARTBEAT_TIMEOUT_MS
#else
#define HEARTBEAT_TIMEOUT_MS 60000
#endif

#ifdef CONFIG_HOOK_LOW_BATTERY_THRESHOLD
#define LOW_BATTERY_THRESHOLD CONFIG_HOOK_LOW_BATTERY_THRESHOLD
#else
#define LOW_BATTERY_THRESHOLD 2000
#endif

#ifdef CONFIG_HOOK_DEFAULT_ALARM_VOLUME
#define DEFAULT_VOLUME ((uint16_t)CONFIG_HOOK_DEFAULT_ALARM_VOLUME)
#else
#define DEFAULT_VOLUME 0x001EU
#endif

#define BMS_SAMPLE_BUFFER_SIZE 30U
#define BMS_SAMPLE_VALID_COUNT 20U

/* The hook is the Modbus RTU slave of the crane host controller. */
#ifdef CONFIG_HOOK_MODBUS_SLAVE_ID
#define FORWARD_SLAVE_ADDR ((uint8_t)CONFIG_HOOK_MODBUS_SLAVE_ID)
#else
#define FORWARD_SLAVE_ADDR 1U
#endif

/* Number of holding registers served on the host RS485 bus. */
#define REGS_TOTAL_NUM 256U

/* ------------------------------------------------------------------ */
/* RTC / standby                                                       */
/* ------------------------------------------------------------------ */
#ifdef CONFIG_HOOK_POWER_ON_TIME_HHMM
#define POWER_ON_DEFAULT ((uint16_t)CONFIG_HOOK_POWER_ON_TIME_HHMM)
#else
#define POWER_ON_DEFAULT ((uint16_t)((6U << 8) | 0U))   /* 06:00 */
#endif

#ifdef CONFIG_HOOK_POWER_OFF_TIME_HHMM
#define POWER_OFF_DEFAULT ((uint16_t)CONFIG_HOOK_POWER_OFF_TIME_HHMM)
#else
#define POWER_OFF_DEFAULT ((uint16_t)((21U << 8) | 0U)) /* 21:00 */
#endif

/* ------------------------------------------------------------------ */
/* External sound & light alarm module (Modbus RTU slave 1 on uart7)   */
/* ------------------------------------------------------------------ */
#define SLAVE_LED_ID 1U

/* External module registers */
#define REG_LED_CTRL         0x00C2U
#define REG_SOUND_LIGHT_CTRL 0x0003U
#define REG_VOLUME_CTRL      0x0006U
#define REG_SOUND_STOP       0x000EU

/* External module commands */
#define CMD_LED_ON          0x0041U /* solid on (software blink uses this) */
#define CMD_LED_SLOW_FLASH  0x0051U
#define CMD_LED_BURST_FLASH 0x0061U
#define CMD_LED_OFF         0x0060U
#define CMD_SOUND_7M        0x0008U
#define CMD_SOUND_3M        0x0009U
#define CMD_SOUND_STOP      0x0000U

/* ------------------------------------------------------------------ */
/* BMS (battery management system, Modbus RTU slave 2 on usart6)       */
/* ------------------------------------------------------------------ */
#define SLAVE_BMS_ID 2U

/* BMS registers */
#define REG_BATTERY_LEVEL    0x0000U
#define REG_TOTAL_CURRENT    0x0001U
#define REG_TOTAL_VOLTAGE    0x0002U
#define REG_REMAIN_DISCHARGE 0x0007U
#define REG_REMAIN_CHARGE    0x0008U
#define REG_IS_CHARGING      0x000BU
#define REG_PROTECT_STATUS   0x0062U

/* ------------------------------------------------------------------ */
/* Error codes (mirrored into holding register REG_ERROR_CODE)         */
/* The heart LED blinks "code" times per second.                       */
/* ------------------------------------------------------------------ */
typedef enum {
	ERR_NONE              = 0x00, /* normal: 1 s on / 1 s off */
	ERR_HEARTBEAT_TIMEOUT = 0x01, /* host heartbeat lost (>60 s) */
	ERR_LED_OFFLINE       = 0x02, /* sound & light module offline */
	ERR_BMS_READ_FAIL     = 0x04, /* BMS read failure */
} hook_error_code_t;

/* ------------------------------------------------------------------ */
/* Work modes (mirrored into holding register STATUS_WORK_MODE)        */
/* ------------------------------------------------------------------ */
typedef enum {
	MODE_DEVICE_STANDBY = 0,
	MODE_COM_WORKING    = 1,
	MODE_LOW_BATTERY    = 2,
	DEVICE_ERROR        = 3,
} hook_work_mode_t;

/* ------------------------------------------------------------------ */
/* Task alive mask (watchdog check-in, replaces g_task_alive_flags)    */
/*                                                                     */
/* A check-in means "this thread is running", NOT "this thread had     */
/* work to do": every thread reports its bit unconditionally at the top */
/* of its loop, also while the work itself is parked (soft standby).   */
/* Mode dependent pauses are handled by hook_health_sleep_update() and */
/* hook_health_ota_gate(), never by skipping the check-in.             */
/* ------------------------------------------------------------------ */
#define HOOK_TASK_ALIVE_ALARM  (1U << 0)
#define HOOK_TASK_ALIVE_BMS    (1U << 1)
#define HOOK_TASK_ALIVE_HEARTBEAT (1U << 2)
#define HOOK_TASK_ALIVE_RTC    (1U << 3)
#define HOOK_TASK_ALIVE_4G     (1U << 4)
#define HOOK_TASK_ALIVE_ALL    (HOOK_TASK_ALIVE_ALARM | HOOK_TASK_ALIVE_BMS | \
				HOOK_TASK_ALIVE_HEARTBEAT | HOOK_TASK_ALIVE_RTC | \
				HOOK_TASK_ALIVE_4G)

#ifdef __cplusplus
}
#endif

#endif /* HOOK_SYSTEM_DEF_H */
