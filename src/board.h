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
static const struct gpio_dt_spec modem_power_gpio = GPIO_SPEC_ALIAS(modem_power);
static const struct gpio_dt_spec modem_reset_gpio = GPIO_SPEC_ALIAS(modem_reset);
static const struct gpio_dt_spec modem_reload_gpio = GPIO_SPEC_ALIAS(modem_reload);

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
