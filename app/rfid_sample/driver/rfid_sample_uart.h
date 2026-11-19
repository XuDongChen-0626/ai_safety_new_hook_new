#ifndef RFID_SAMPLE_UART_H_
#define RFID_SAMPLE_UART_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define RFID_SAMPLE_RX_BLOCK_SIZE 32U
#define RFID_SAMPLE_UART_TX_MAX_SIZE 9U

/* Private transport contract: generation changes on any known stream gap. */
struct rfid_sample_rx_block {
	uint64_t received_ms;
	uint32_t generation;
	uint8_t length;
	uint8_t data[RFID_SAMPLE_RX_BLOCK_SIZE];
};

struct rfid_sample_uart {
	const struct device *device;
	struct k_msgq *queue;
	atomic_t generation;
	atomic_t rx_bytes;
	atomic_t dropped_bytes;
	atomic_t errors;
	atomic_t fatal_error;
	atomic_t started;
	struct k_mutex tx_mutex;
	struct k_spinlock tx_lock;
	struct k_sem tx_done;
	uint8_t tx_data[RFID_SAMPLE_UART_TX_MAX_SIZE];
	size_t tx_length;
	size_t tx_offset;
	int tx_result;
	bool tx_active;
};

/* Zero-initialized, exclusive context and queue must outlive reception.
 * Start once from a thread. The ISR only drains/copies bytes, never parses.
 * No DE control, power control or protocol handling is performed here.
 */
int rfid_sample_uart_start(struct rfid_sample_uart *uart,
			 const struct device *device, struct k_msgq *queue);

/* Thread only; timeout must be finite and nonzero. Concurrent sends return
 * -EBUSY. The bytes are copied into the context before enabling TX IRQ.
 * Success means all bytes entered the UART FIFO, not a device acknowledgement
 * or completion of the final stop bit. On timeout/error, already queued bytes
 * may still leave the UART; remaining bytes are cancelled without stopping RX.
 */
int rfid_sample_uart_send(struct rfid_sample_uart *uart, const uint8_t *data,
			 size_t len, k_timeout_t timeout);

int rfid_sample_uart_set_baudrate(struct rfid_sample_uart *uart,
				 uint32_t baudrate);

#endif
