/*
 * hook_4g_uart.h - asynchronous DMA transport for the usr.cn 4G module.
 */
#ifndef HOOK_4G_UART_H
#define HOOK_4G_UART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum logical frame accepted by the TX queue. The largest normal frame is
 * a 2-byte usr.cn topic prefix plus a 512-byte MQTT reply.
 */
#define HOOK_4G_UART_TX_MAX_FRAME 576U

typedef int32_t (*hook_4g_uart_rx_cb_t)(uint8_t *buf, uint16_t len);

/*
 * Configure usart1 for async DMA RX/TX, start the continuous two-buffer RX
 * stream and create the serialised TX worker.
 *
 * @param rx_cb receives each idle-delimited block (20 ms) or full DMA block.
 *              It is called from ISR or system-workqueue context and must be
 *              ISR safe.
 */
int hook_4g_uart_init(hook_4g_uart_rx_cb_t rx_cb);

/*
 * Queue a complete logical frame for transmission. The source buffer may be
 * reused as soon as this returns; the worker owns a private DMA-safe copy.
 *
 * @return 0 on success, -EMSGSIZE when the frame is too long, -ENOMEM when the
 *         queue is full, or another negative errno on failure.
 */
int hook_4g_uart_send(const uint8_t *data, uint16_t len);

/*
 * Abort the frame on the wire and discard all queued frames. Used before a
 * module power-cycle so commands from the old session cannot leak into the new
 * one.
 */
void hook_4g_uart_flush_tx(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_4G_UART_H */
