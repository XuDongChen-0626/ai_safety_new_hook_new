/*
 * pwr_stm32.c - STM32H743 STANDBY control.
 */
#include "pwr_stm32.h"

#include <soc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(hook_pwr, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * On STM32H7 the RTC alarm event is routed to EXTI line 17, and that line is
 * also the wake-up source used when the MCU is in STANDBY. Zephyr's RTC
 * driver registers the line through the EXTI interrupt controller, but the
 * interrupt mask has to be set for the event to bring the core out of
 * STANDBY, so it is forced here as well.
 */
#define PWR_RTC_ALARM_EXTI_LINE 17U

int pwr_wakeup_from_standby(void)
{
	return (PWR->CPUCR & PWR_CPUCR_SBF) ? 1 : 0;
}

void pwr_clear_standby_flag(void)
{
	PWR->CPUCR |= PWR_CPUCR_CSSF;
}

void pwr_clear_wakeup_flags(void)
{
	PWR->WKUPCR |= (PWR_WKUPCR_WKUPC1 | PWR_WKUPCR_WKUPC2 |
			PWR_WKUPCR_WKUPC3 | PWR_WKUPCR_WKUPC4 |
			PWR_WKUPCR_WKUPC5 | PWR_WKUPCR_WKUPC6);
}

void pwr_clear_rtc_alarm_b_flag(void)
{
	RTC->ISR &= ~RTC_ISR_ALRBF;
}

void pwr_enter_standby(void)
{
	/* Unmask the RTC alarm wake-up line before powering the domains down. */
	EXTI->IMR1 |= BIT(PWR_RTC_ALARM_EXTI_LINE);

	/* D1/D2/D3 power down deepsleep: with SLEEPDEEP this selects STANDBY. */
	PWR->CPUCR |= PWR_CPUCR_PDDS_D1 | PWR_CPUCR_PDDS_D2 | PWR_CPUCR_PDDS_D3;
	SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
	__DSB();
	__ISB();
	__WFI();

	/*
	 * Reached only if a pending interrupt prevented STANDBY entry: clear the
	 * deepsleep bits so the next WFI does not immediately power the MCU down
	 * again.
	 */
	PWR->CPUCR &= ~(PWR_CPUCR_PDDS_D1 | PWR_CPUCR_PDDS_D2 | PWR_CPUCR_PDDS_D3);
	SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
}
