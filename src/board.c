/*
 * board.c - board initialization and application entry glue.
 *
 * Replaces Core/Src/gpio.c, Core/Src/main.c (application part) and the task
 * creation part of application/app_hook_task.c of the CubeMX firmware.
 */
#include "board.h"

#include "hook_task.h"
#include "system_def.h"
#include "uart_manage_port.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hook_board, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * Initial output levels copied from crane_ai_satefy_hook.ioc / MX_GPIO_Init():
 *   3V3 / 5V / 4G rails on, GPS enabled, heart LED on, relay on,
 *   activity LED off, 4G reset/reload released, PWD_LED off.
 *
 * Every pin must be configured as an output here: the devicetree node only
 * describes the pins (gpio-leds container) and the LED GPIO driver is not
 * compiled in, so the pins are left in their reset state otherwise (this was
 * the cause of "5V rail off / all RS485 modules unreachable").
 */
void board_gpio_init(void)
{
	/* Power rails / enables: active at boot. */
	(void)gpio_pin_configure_dt(&power_3v_gpio, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&power_5v_gpio, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&modem_power_gpio, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&gps_en_gpio, GPIO_OUTPUT_ACTIVE);

	/* Relay energised and heart LED on (the LED animation takes over). */
	(void)gpio_pin_configure_dt(&relay_en_gpio, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&heartbeat_led_gpio, GPIO_OUTPUT_ACTIVE);

	/* PWD_LED (PB3) starts low: it mirrors the Modbus flash-light command
	 * register (driven by app/hook_alarm), not the relay state.
	 */
	(void)gpio_pin_configure_dt(&pwr_led_gpio, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&activity_led_gpio, GPIO_OUTPUT_INACTIVE);

	/* 4G module reset / reload released (active high pulse only). */
	(void)gpio_pin_configure_dt(&modem_reset_gpio, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&modem_reload_gpio, GPIO_OUTPUT_INACTIVE);
}

void init_app(void)
{
	LOG_INF("[BOOT] craner_ai_safety_hook (Zephyr)");
	LOG_INF("fw time: %s %s", __DATE__, __TIME__);

	board_gpio_init();
	init_uart_manage();
	init_app_hook_task();
}
