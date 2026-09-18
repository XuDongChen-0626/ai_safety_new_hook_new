/*
 * uart_manage_port.c - Zephyr UART table for the hook controller.
 */
#include "uart_manage_port.h"

#include "system_def.h"
#include "uart_manage.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

LOG_MODULE_REGISTER(hook_uart, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * The 4G channel carries whole Ymodem packets (up to 1 KiB data + header)
 * inside a single usr.cn message, so its receive buffer has to hold the
 * complete frame; the old bootloader used 2048 B for the same link.
 */
#define HOOK_UART_RECV_BUFFER_SIZE  256U
#define HOOK_MODEM_RECV_BUFFER_SIZE 2048U
#define HOOK_UART_SEND_BUFFER_SIZE  256U
#define HOOK_UART_SEND_FIFO_SIZE    256U
#define HOOK_UART_PROCESS_BUFFER    1024U

static uint8_t modem_recv_buff[HOOK_MODEM_RECV_BUFFER_SIZE];
/* Bounce buffer: the idle-timer handler copies each received frame here so
 * the UART ISR cannot overwrite it while the 4G layer processes it.
 */
static uint8_t modem_recv_buff_alt[HOOK_MODEM_RECV_BUFFER_SIZE];
static uint8_t modem_send_buff[HOOK_UART_SEND_BUFFER_SIZE];
static uint8_t modem_send_fifo_buff[HOOK_UART_SEND_FIFO_SIZE];
static uint8_t modem_process_buff[HOOK_UART_PROCESS_BUFFER];

static const uart_inferface_t hook_uart_manage_table[] = {
	{
		.name = "4g",
		.uart_dev = DEVICE_DT_GET(DT_NODELABEL(usart1)),
		.baudrate = 115200U,
		.rx_idle_ms = 20U,
		.recv_buffer = modem_recv_buff,
		.recv_buffer_size = sizeof(modem_recv_buff),
		.recv_buffer_alt = modem_recv_buff_alt,
		.process_buffer = modem_process_buff,
		.process_buffer_size = sizeof(modem_process_buff),
		.recv_callback = hook_4g_recv_callback,
		.send_buffer = modem_send_buff,
		.send_buffer_size = sizeof(modem_send_buff),
		.send_fifo_buffer = modem_send_fifo_buff,
		.send_fifo_size = sizeof(modem_send_fifo_buff),
		.send_callback = NULL,
	},
};

int32_t hook_console_send(const uint8_t *buf, uint16_t len)
{
	const struct shell *sh = shell_backend_uart_get_ptr();

	if ((buf == NULL) || (len == 0U)) {
		return -1;
	}

	/*
	 * Print through the shell backend instead of printk(): the shell owns
	 * uart5 and its line editor, and a raw printk() is overwritten by the
	 * next prompt redraw.
	 *
	 * Everything must go out in ONE call: outside a command context the
	 * shell erases the current line, prints, and then restores the prompt,
	 * so splitting a long answer over several calls would erase the line
	 * heads it has just printed.
	 */
	if (sh != NULL) {
		shell_fprintf(sh, SHELL_NORMAL, "%.*s", (int)len, (const char *)buf);
	} else {
		printk("%.*s", (int)len, (const char *)buf);
	}

	return 0;
}

void init_uart_manage(void)
{
	for (uint16_t i = 0U;
	     i < (uint16_t)(sizeof(hook_uart_manage_table) / sizeof(hook_uart_manage_table[0]));
	     ++i) {
		const uart_inferface_t *entry = &hook_uart_manage_table[i];
		int ret;

		ret = uart_manage_register_interface((uart_inferface_t *)entry);
		if (ret != UART_MANAGE_OK) {
			LOG_ERR("uart_manage register %s failed: %d", entry->name, ret);
			continue;
		}

		ret = uart_manage_enable_recv(entry->uart_dev);
		if (ret != UART_MANAGE_OK) {
			LOG_ERR("uart_manage enable %s failed: %d", entry->name, ret);
			continue;
		}

		LOG_INF("uart_manage init %s ok", entry->name);
	}
}
