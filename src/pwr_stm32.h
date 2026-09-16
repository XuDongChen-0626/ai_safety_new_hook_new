/*
 * pwr_stm32.h - STM32H743 STANDBY helpers.
 *
 * Zephyr has no direct "enter standby" API, so this small module talks to the
 * PWR/RTC/EXTI registers through the CMSIS device header, exactly like the
 * original firmware did with the HAL.
 */
#ifndef HOOK_PWR_STM32_H
#define HOOK_PWR_STM32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the current boot came from STANDBY (RTC alarm B wake-up). */
int pwr_wakeup_from_standby(void);

void pwr_clear_standby_flag(void);
void pwr_clear_wakeup_flags(void);
void pwr_clear_rtc_alarm_b_flag(void);

/*
 * Arm the RTC alarm wake-up line and enter STM32 STANDBY.
 * Returns only when standby could not be entered.
 */
void pwr_enter_standby(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_PWR_STM32_H */
