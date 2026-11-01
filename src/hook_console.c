/*
 * hook_console.c - debug console output shared by the shell and 4G paths.
 */
#include "hook_console.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

int32_t hook_console_send(const uint8_t *buf, uint16_t len)
{
	const struct shell *sh = shell_backend_uart_get_ptr();

	if ((buf == NULL) || (len == 0U)) {
		return -1;
	}

	/*
	 * The shell owns uart5 and its line editor. A raw printk() is overwritten
	 * by the next prompt redraw, so route the whole answer through the shell
	 * in one call.
	 */
	if (sh != NULL) {
		shell_fprintf(sh, SHELL_NORMAL, "%.*s", (int)len,
			      (const char *)buf);
	} else {
		printk("%.*s", (int)len, (const char *)buf);
	}

	return 0;
}
