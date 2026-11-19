#include "rfid_sample_uart.h"

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/uart.h>

static void transmit_finish_locked(struct rfid_sample_uart *uart, int result)
{
	uart_irq_tx_disable(uart->device);
	uart->tx_active = false;
	uart->tx_length = 0U;
	uart->tx_offset = 0U;
	uart->tx_result = result;
	k_sem_give(&uart->tx_done);
}

static void transport_failed(struct rfid_sample_uart *uart, int error)
{
	k_spinlock_key_t key = k_spin_lock(&uart->tx_lock);

	atomic_set(&uart->fatal_error, error);
	atomic_inc(&uart->errors);
	atomic_inc(&uart->generation);
	uart_irq_rx_disable(uart->device);
	uart_irq_err_disable(uart->device);
	if (uart->tx_active) {
		transmit_finish_locked(uart, error);
	} else {
		uart_irq_tx_disable(uart->device);
	}
	k_spin_unlock(&uart->tx_lock, key);
}

static void transmit_irq(struct rfid_sample_uart *uart)
{
	k_spinlock_key_t key = k_spin_lock(&uart->tx_lock);

	if (!uart->tx_active) {
		k_spin_unlock(&uart->tx_lock, key);
		return;
	}
	int ready = uart_irq_tx_ready(uart->device);

	if (ready < 0) {
		atomic_inc(&uart->errors);
		transmit_finish_locked(uart, ready);
	} else if (ready > 0) {
		size_t remaining = uart->tx_length - uart->tx_offset;
		int count = uart_fifo_fill(uart->device,
					  &uart->tx_data[uart->tx_offset],
					  (int)remaining);

		if (count <= 0 || (size_t)count > remaining) {
			atomic_inc(&uart->errors);
			transmit_finish_locked(uart, count < 0 ? count : -EIO);
		} else {
			uart->tx_offset += (size_t)count;
			if (uart->tx_offset == uart->tx_length) {
				transmit_finish_locked(uart, 0);
			}
		}
	}
	k_spin_unlock(&uart->tx_lock, key);
}

static bool receive_fifo(struct rfid_sample_uart *uart, bool discard)
{
	int count;

	do {
		struct rfid_sample_rx_block block = {0};

		count = uart_fifo_read(uart->device, block.data, sizeof(block.data));
		if (count < 0) {
			transport_failed(uart, count);
			return false;
		}
		if (count == 0) {
			break;
		}
		atomic_add(&uart->rx_bytes, count);
		int errors = uart_err_check(uart->device);

		if (errors < 0) {
			atomic_add(&uart->dropped_bytes, count);
			transport_failed(uart, errors);
			return false;
		}
		if (errors > 0) {
			discard = true;
			atomic_inc(&uart->errors);
			atomic_inc(&uart->generation);
		}
		if (discard) {
			atomic_add(&uart->dropped_bytes, count);
			continue;
		}

		block.received_ms = (uint64_t)k_uptime_get();
		block.generation = (uint32_t)atomic_get(&uart->generation);
		block.length = (uint8_t)count;
		if (k_msgq_put(uart->queue, &block, K_NO_WAIT) != 0) {
			atomic_add(&uart->dropped_bytes, count);
			atomic_inc(&uart->generation);
		}
	} while (count == (int)RFID_SAMPLE_RX_BLOCK_SIZE);
	return true;
}

static void receive_irq(const struct device *device, void *user_data)
{
	struct rfid_sample_uart *uart = user_data;
	int rc = uart_irq_update(device);

	if (rc < 0) {
		transport_failed(uart, rc);
		return;
	}
	if (rc == 0) {
		return;
	}

	int errors = uart_err_check(device);
	bool discard = errors > 0;

	if (errors < 0) {
		transport_failed(uart, errors);
		return;
	}
	if (discard) {
		atomic_inc(&uart->errors);
		atomic_inc(&uart->generation);
	}

	rc = uart_irq_rx_ready(device);
	if (rc < 0) {
		transport_failed(uart, rc);
		return;
	}
	if (rc > 0 && !receive_fifo(uart, discard)) {
		return;
	}
	transmit_irq(uart);
}

int rfid_sample_uart_start(struct rfid_sample_uart *uart,
			 const struct device *device, struct k_msgq *queue)
{
	if (uart == NULL || device == NULL || queue == NULL) {
		return -EINVAL;
	}
	if (!device_is_ready(device)) {
		return -ENODEV;
	}

	uart->device = device;
	uart->queue = queue;
	uart_irq_rx_disable(device);
	uart_irq_tx_disable(device);
	uart_irq_err_disable(device);
	k_mutex_init(&uart->tx_mutex);
	k_sem_init(&uart->tx_done, 0, 1);
	memset(&uart->tx_lock, 0, sizeof(uart->tx_lock));
	uart->tx_active = false;
	uart->tx_length = 0U;
	uart->tx_offset = 0U;
	uart->tx_result = 0;

	int rc = uart_irq_callback_user_data_set(device, receive_irq, uart);

	if (rc < 0) {
		return rc;
	}
	rc = uart_err_check(device);
	if (rc < 0) {
		return rc;
	}
	atomic_set(&uart->started, 1);
	uart_irq_err_enable(device);
	uart_irq_rx_enable(device);
	return 0;
}

int rfid_sample_uart_send(struct rfid_sample_uart *uart, const uint8_t *data,
			 size_t len, k_timeout_t timeout)
{
	if (uart == NULL || data == NULL || len == 0U ||
	    len > RFID_SAMPLE_UART_TX_MAX_SIZE ||
	    K_TIMEOUT_EQ(timeout, K_FOREVER) || K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (!atomic_get(&uart->started)) {
		return -ENODEV;
	}
	if (k_mutex_lock(&uart->tx_mutex, K_NO_WAIT) != 0) {
		return -EBUSY;
	}

	k_sem_reset(&uart->tx_done);
	k_spinlock_key_t key = k_spin_lock(&uart->tx_lock);
	int rc = (int)atomic_get(&uart->fatal_error);

	if (rc == 0) {
		memcpy(uart->tx_data, data, len);
		uart->tx_length = len;
		uart->tx_offset = 0U;
		uart->tx_result = -EINPROGRESS;
		uart->tx_active = true;
		uart_irq_tx_enable(uart->device);
	}
	k_spin_unlock(&uart->tx_lock, key);
	if (rc != 0) {
		k_mutex_unlock(&uart->tx_mutex);
		return rc;
	}

	rc = k_sem_take(&uart->tx_done, timeout);
	key = k_spin_lock(&uart->tx_lock);
	if (uart->tx_active) {
		transmit_finish_locked(uart, rc == -EAGAIN ? -ETIMEDOUT : -EIO);
	}
	rc = uart->tx_result;
	k_spin_unlock(&uart->tx_lock, key);
	k_mutex_unlock(&uart->tx_mutex);
	return rc;
}

int rfid_sample_uart_set_baudrate(struct rfid_sample_uart *uart,
				 uint32_t baudrate)
{
	struct uart_config config;
	int rc;

	if (uart == NULL || (baudrate != 9600U && baudrate != 115200U)) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (!atomic_get(&uart->started)) {
		return -ENODEV;
	}
	if (k_mutex_lock(&uart->tx_mutex, K_NO_WAIT) != 0) {
		return -EBUSY;
	}

	k_spinlock_key_t key = k_spin_lock(&uart->tx_lock);
	if (uart->tx_active) {
		k_spin_unlock(&uart->tx_lock, key);
		k_mutex_unlock(&uart->tx_mutex);
		return -EBUSY;
	}
	uart_irq_rx_disable(uart->device);
	uart_irq_err_disable(uart->device);
	k_spin_unlock(&uart->tx_lock, key);

	rc = uart_config_get(uart->device, &config);
	if (rc == 0) {
		config.baudrate = baudrate;
		config.parity = UART_CFG_PARITY_NONE;
		config.stop_bits = UART_CFG_STOP_BITS_1;
		config.data_bits = UART_CFG_DATA_BITS_8;
		config.flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
		rc = uart_configure(uart->device, &config);
	}

	atomic_inc(&uart->generation);
	uart_irq_err_enable(uart->device);
	uart_irq_rx_enable(uart->device);
	k_mutex_unlock(&uart->tx_mutex);
	return rc;
}
