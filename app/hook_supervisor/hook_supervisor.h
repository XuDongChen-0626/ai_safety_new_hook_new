/*
 * hook_supervisor.h - heart LED animation and work-mode aggregation.
 *
 * Ported from application/app_sys_supervisor.c.
 */
#ifndef HOOK_SUPERVISOR_H
#define HOOK_SUPERVISOR_H

#include <zephyr/drivers/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error code is rendered as a blink pattern on the heart LED (PD10). */
typedef enum {
	HOOK_LED_MODE_NORMAL = 0,     /* 1 s on / 1 s off                */
	HOOK_LED_MODE_BURST,          /* fast flash (multiple errors)    */
	HOOK_LED_MODE_BLINK_PAUSE,    /* N flashes then 2 s pause        */
} hook_led_mode_t;

typedef struct {
	const struct gpio_dt_spec *led;
	uint16_t step_period_ms;
	uint16_t cycle_step;
	hook_led_mode_t last_mode;
	uint8_t last_count;
} hook_led_anim_t;

void hook_supervisor_start(void);

void hook_led_animation_step(hook_led_anim_t *ctx, hook_led_mode_t mode,
			     uint8_t blink_count);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_SUPERVISOR_H */
