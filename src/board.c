/*
 * board.c - board initialization and application entry glue.
 *
 * Replaces Core/Src/gpio.c, Core/Src/main.c (application part) and the task
 * creation part of application/app_hook_task.c of the CubeMX firmware.
 */
#include "board.h"

#include "hook_task.h"
#include "system_def.h"

#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hook_board, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * Initial output levels copied from crane_ai_satefy_hook.ioc / MX_GPIO_Init():
 *   3V3 / 5V / 4G rails on where the board has controllable switches, heart
 *   LED on, alarm-light supply on, 4G reset/reload released, PWD_LED off.
 *
 * Pins whose state is not needed before application startup are configured
 * here. Board power enables needed earlier are handled by
 * board_early_power_init() and are intentionally not written twice.
 */
void board_gpio_init(void)
{
	/* Power rails / enables: active at boot. */
#if DT_HAS_ALIAS(power_3v)
	(void)gpio_pin_configure_dt(&power_3v_gpio, GPIO_OUTPUT_ACTIVE);
#endif
#if DT_HAS_ALIAS(power_5v)
	(void)gpio_pin_configure_dt(&power_5v_gpio, GPIO_OUTPUT_ACTIVE);
#endif

	/* Alarm-light supply on and heart LED on (the LED animation takes over). */
	(void)gpio_pin_configure_dt(&alarm_power_en_gpio, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&heartbeat_led_gpio, GPIO_OUTPUT_ACTIVE);

	/* PWD LED/supply starts low and is driven by app/hook_alarm from the
	 * combined light-command and siren state.
	 */
	(void)gpio_pin_configure_dt(&pwr_led_gpio, GPIO_OUTPUT_INACTIVE);

	/* 4G module reset / reload released (active high pulse only). */
	(void)gpio_pin_configure_dt(&modem_reset_gpio, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&modem_reload_gpio, GPIO_OUTPUT_INACTIVE);
}

/*
 * V3 has mandatory field-power enables that must be asserted before any
 * application thread or device probing starts. PF3_BMQ_EN powers the hook
 * connector board itself, so leaving it in its reset state makes every
 * downstream device inaccessible.
 *
 * The old board has no such aliases; its rails continue to be enabled by
 * board_gpio_init() from main().
 */
static int board_early_power_init(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(rs485_usart6))
#if DT_NODE_HAS_PROP(DT_NODELABEL(rs485_usart6), de_gpios)
	if (!gpio_is_ready_dt(&rs485_usart6_de_gpio)) {
		LOG_ERR("USART6 RS485 direction GPIO is not ready");
		return -ENODEV;
	}
	(void)gpio_pin_configure_dt(&rs485_usart6_de_gpio,
				    GPIO_OUTPUT_INACTIVE);
#endif
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(rs485_uart7))
#if DT_NODE_HAS_PROP(DT_NODELABEL(rs485_uart7), de_gpios)
	if (!gpio_is_ready_dt(&rs485_uart7_de_gpio)) {
		LOG_ERR("UART7 RS485 direction GPIO is not ready");
		return -ENODEV;
	}
	(void)gpio_pin_configure_dt(&rs485_uart7_de_gpio,
				    GPIO_OUTPUT_INACTIVE);
#endif
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(rs485_uart8))
#if DT_NODE_HAS_PROP(DT_NODELABEL(rs485_uart8), de_gpios)
	if (!gpio_is_ready_dt(&rs485_uart8_de_gpio)) {
		LOG_ERR("UART8 RS485 direction GPIO is not ready");
		return -ENODEV;
	}
	(void)gpio_pin_configure_dt(&rs485_uart8_de_gpio,
				    GPIO_OUTPUT_INACTIVE);
#endif
#endif

#if DT_HAS_ALIAS(bmq_en)
	if (!gpio_is_ready_dt(&bmq_en_gpio)) {
		LOG_ERR("BMQ power enable GPIO is not ready");
		return -ENODEV;
	}
	(void)gpio_pin_configure_dt(&bmq_en_gpio, GPIO_OUTPUT_ACTIVE);
#endif

#if DT_HAS_ALIAS(rs485_en)
	if (!gpio_is_ready_dt(&rs485_en_gpio)) {
		LOG_ERR("RS485 power enable GPIO is not ready");
		return -ENODEV;
	}
	(void)gpio_pin_configure_dt(&rs485_en_gpio, GPIO_OUTPUT_ACTIVE);
#endif

#if DT_HAS_ALIAS(modem_enable)
	if (!gpio_is_ready_dt(&modem_enable_gpio)) {
		LOG_ERR("4G module enable GPIO is not ready");
		return -ENODEV;
	}
	(void)gpio_pin_configure_dt(&modem_enable_gpio, GPIO_OUTPUT_ACTIVE);
#endif

	if (!gpio_is_ready_dt(&modem_power_gpio)) {
		LOG_ERR("4G module power GPIO is not ready");
		return -ENODEV;
	}
	(void)gpio_pin_configure_dt(&modem_power_gpio, GPIO_OUTPUT_ACTIVE);

	return 0;
}

SYS_INIT(board_early_power_init, POST_KERNEL, 45);

void init_app(void)
{
	LOG_INF("[BOOT] craner_ai_safety_hook (Zephyr)");
	LOG_INF("fw time: %s %s", __DATE__, __TIME__);

	board_gpio_init();
	init_app_hook_task();
}
