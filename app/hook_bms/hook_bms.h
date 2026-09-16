/*
 * hook_bms.h - battery management system polling (usart6).
 *
 * Ported from the BMS half of application/app_bms_alarm.c
 * (modbus_bms_handle).
 */
#ifndef HOOK_BMS_H
#define HOOK_BMS_H

#ifdef __cplusplus
extern "C" {
#endif

void hook_bms_start(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_BMS_H */
