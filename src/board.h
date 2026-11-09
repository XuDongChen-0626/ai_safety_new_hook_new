/*
 * board.h - board GPIO aliases (devicetree driven) and application entry.
 */
#ifndef HOOK_BOARD_H
#define HOOK_BOARD_H

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_SPEC_ALIAS(alias) GPIO_DT_SPEC_GET(DT_ALIAS(alias), gpios)

static const struct gpio_dt_spec heartbeat_led_gpio = GPIO_SPEC_ALIAS(heartbeat_led);
static const struct gpio_dt_spec pwr_led_gpio = GPIO_SPEC_ALIAS(pwr_led);
static const struct gpio_dt_spec alarm_power_en_gpio = GPIO_SPEC_ALIAS(alarm_power_en);
#if DT_HAS_ALIAS(power_3v)
static const struct gpio_dt_spec power_3v_gpio = GPIO_SPEC_ALIAS(power_3v);
#endif
#if DT_HAS_ALIAS(power_5v)
static const struct gpio_dt_spec power_5v_gpio = GPIO_SPEC_ALIAS(power_5v);
#endif
static const struct gpio_dt_spec modem_power_gpio = GPIO_SPEC_ALIAS(modem_power);
static const struct gpio_dt_spec modem_reset_gpio = GPIO_SPEC_ALIAS(modem_reset);
static const struct gpio_dt_spec modem_reload_gpio = GPIO_SPEC_ALIAS(modem_reload);
#if DT_HAS_ALIAS(modem_enable)
static const struct gpio_dt_spec modem_enable_gpio = GPIO_SPEC_ALIAS(modem_enable);
#endif
#if DT_HAS_ALIAS(bmq_en)
static const struct gpio_dt_spec bmq_en_gpio = GPIO_SPEC_ALIAS(bmq_en);
#endif
#if DT_HAS_ALIAS(rs485_en)
static const struct gpio_dt_spec rs485_en_gpio = GPIO_SPEC_ALIAS(rs485_en);
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(rs485_usart6))
#if DT_NODE_HAS_PROP(DT_NODELABEL(rs485_usart6), de_gpios)
static const struct gpio_dt_spec rs485_usart6_de_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(rs485_usart6), de_gpios);
#endif
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(rs485_uart7))
#if DT_NODE_HAS_PROP(DT_NODELABEL(rs485_uart7), de_gpios)
static const struct gpio_dt_spec rs485_uart7_de_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(rs485_uart7), de_gpios);
#endif
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(rs485_uart8))
#if DT_NODE_HAS_PROP(DT_NODELABEL(rs485_uart8), de_gpios)
static const struct gpio_dt_spec rs485_uart8_de_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(rs485_uart8), de_gpios);
#endif
#endif

/* Logical helpers: "on" always means active for the signal. */
#define GPIO_WRITE(spec, on) gpio_pin_set_dt(&(spec), (on) ? 1 : 0)
#define GPIO_READ(spec) gpio_pin_get_dt(&(spec))

/* Milliseconds since boot (the old firmware used get_time_ms()). */
static inline uint32_t hook_get_time_ms(void)
{
	return (uint32_t)k_uptime_get_32();
}

void board_gpio_init(void);
void init_app(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_BOARD_H */
