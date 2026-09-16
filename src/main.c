/*
 * main.c - Zephyr application entry point.
 *
 * The old Core/Src/main.c only configured the clocks/peripherals and then
 * started the FreeRTOS scheduler; in Zephyr the clocks and peripheral
 * instances come from devicetree, so main() only has to bring the
 * application up. The IWDG is owned by the system health watchdog backend.
 */
#include <zephyr/kernel.h>

#include "board.h"

int main(void)
{
	init_app();

	return 0;
}
