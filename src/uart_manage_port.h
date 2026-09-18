/*
 * uart_manage_port.h - hook product UART channel table.
 *
 * Zephyr interrupt-driven channels (component/uart_manage):
 *   "4g"  -> usart1 (PA9/PA10, 115200) : usr.cn transparent 4G module
 *
 * usart3 was the GPS port in the old firmware; this product has no GPS
 * module, so the channel table only carries the 4G link.
 *
 * The RS485 Modbus ports (uart8 slave, usart6 + uart7 masters) and the debug
 * console (uart5) are owned by component/am_modbus and the Zephyr shell.
 */
#ifndef HOOK_UART_MANAGE_PORT_H
#define HOOK_UART_MANAGE_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Send a text reply to the debug console (uart5), used by the AT layer. */
int32_t hook_console_send(const uint8_t *buf, uint16_t len);

/*
 * Receive callback of the "4g" channel: handles the usr.cn framing
 * ("<channel>," prefix), the craner#AT command surface and the binary 0xA5
 * cloud protocol. Implemented in app/hook_4g.
 */
int32_t hook_4g_recv_callback(uint8_t *buf, uint16_t len);

void init_uart_manage(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_UART_MANAGE_PORT_H */
