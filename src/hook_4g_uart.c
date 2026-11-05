/*
 * hook_4g_uart.c - Zephyr UART Async API + DMA transport for usart1.
 *
 * RX uses two non-cached DMA buffers. The driver switches between them on a
 * full buffer and publishes an idle-delimited block after the configured 20 ms
 * receiver timeout. TX uses a bounded queue of non-cached frames and one worker,
 * because the async API requires the source buffer to remain valid until
 * UART_TX_DONE/UART_TX_ABORTED.
 */
#include "hook_4g_uart.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(hook_4g_uart, CONFIG_LOG_DEFAULT_LEVEL);

#define HOOK_4G_UART_NODE       DT_NODELABEL(usart1)
#define HOOK_4G_UART_RX_SIZE    2048U
#define HOOK_4G_UART_RX_IDLE_US (10U * USEC_PER_MSEC)
#define HOOK_4G_UART_TX_DEPTH   16U
#define HOOK_4G_UART_TX_TIMEOUT_US (1U * USEC_PER_SEC)
#define HOOK_4G_UART_TX_WAIT_MS 2000U
#define HOOK_4G_UART_RX_RESTART_DELAY_MS 10U
#define HOOK_4G_UART_RX_RETRY_DELAY_MS 1000U
#define HOOK_4G_UART_INVALID_BUF 0xFFU

struct hook_4g_uart_tx_item {
	uint8_t data[HOOK_4G_UART_TX_MAX_FRAME];
	uint16_t len;
	uint32_t generation;
};

static const struct device *const hook_4g_uart_dev =
	DEVICE_DT_GET(HOOK_4G_UART_NODE);

/* STM32H7 async DMA rejects cached buffers, so both buffers are __nocache. */
static uint8_t __nocache __aligned(32)
	hook_4g_uart_rx_buf[2][HOOK_4G_UART_RX_SIZE];

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(hook_4g_uart_tx_slab, __nocache,
				 ROUND_UP(sizeof(struct hook_4g_uart_tx_item), 32U),
				 HOOK_4G_UART_TX_DEPTH, 32);
K_FIFO_DEFINE(hook_4g_uart_tx_fifo);
K_SEM_DEFINE(hook_4g_uart_tx_done, 0, 1);
K_MUTEX_DEFINE(hook_4g_uart_tx_mutex);

static hook_4g_uart_rx_cb_t hook_4g_uart_rx_cb;
static struct k_work_delayable hook_4g_uart_rx_restart_work;
static struct k_thread hook_4g_uart_tx_thread_data;
K_THREAD_STACK_DEFINE(hook_4g_uart_tx_stack, 2048);

static atomic_t hook_4g_uart_tx_generation;
static volatile bool hook_4g_uart_initialized;
static volatile bool hook_4g_uart_rx_enabled;
static volatile bool hook_4g_uart_rx_restarting;
static volatile uint8_t hook_4g_uart_rx_current_buf;
static volatile uint8_t hook_4g_uart_rx_next_buf =
	HOOK_4G_UART_INVALID_BUF;
static volatile bool hook_4g_uart_tx_in_flight;
static volatile int32_t hook_4g_uart_tx_result;

static int hook_4g_uart_enable_rx(void)
{
	int ret;

	hook_4g_uart_rx_current_buf = 0U;
	hook_4g_uart_rx_next_buf = HOOK_4G_UART_INVALID_BUF;
	hook_4g_uart_rx_enabled = false;

	ret = uart_rx_enable(hook_4g_uart_dev, hook_4g_uart_rx_buf[0],
			     sizeof(hook_4g_uart_rx_buf[0]),
			     HOOK_4G_UART_RX_IDLE_US);
	if (ret == 0) {
		hook_4g_uart_rx_enabled = true;
		LOG_INF("4G UART async RX enabled (2 x %u B DMA, idle %u ms)",
			(unsigned int)HOOK_4G_UART_RX_SIZE,
			(unsigned int)(HOOK_4G_UART_RX_IDLE_US / USEC_PER_MSEC));
	} else {
		LOG_ERR("4G UART async RX enable failed: %d", ret);
	}

	return ret;
}

static void hook_4g_uart_rx_restart(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	if (!hook_4g_uart_initialized) {
		return;
	}

	hook_4g_uart_rx_restarting = true;

	if (hook_4g_uart_rx_enabled) {
		(void)uart_rx_disable(hook_4g_uart_dev);
	}

	hook_4g_uart_rx_enabled = false;
	ret = hook_4g_uart_enable_rx();
	hook_4g_uart_rx_restarting = false;

	if (ret != 0) {
		(void)k_work_reschedule(&hook_4g_uart_rx_restart_work,
					K_MSEC(HOOK_4G_UART_RX_RETRY_DELAY_MS));
	}
}

static void hook_4g_uart_event(const struct device *dev,
			       struct uart_event *event, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	switch (event->type) {
	case UART_RX_RDY:
		if ((hook_4g_uart_rx_cb != NULL) && (event->data.rx.len > 0U)) {
			size_t len = event->data.rx.len;

			if (len > UINT16_MAX) {
				len = UINT16_MAX;
			}

			(void)hook_4g_uart_rx_cb(
				event->data.rx.buf + event->data.rx.offset,
				(uint16_t)len);
		}
		break;

	case UART_RX_BUF_REQUEST: {
		uint8_t next = (uint8_t)(hook_4g_uart_rx_current_buf ^ 1U);
		int ret;

		if (hook_4g_uart_rx_next_buf != HOOK_4G_UART_INVALID_BUF) {
			LOG_ERR("4G UART RX buffer request while one is armed");
			break;
		}

		ret = uart_rx_buf_rsp(hook_4g_uart_dev,
				      hook_4g_uart_rx_buf[next],
				      sizeof(hook_4g_uart_rx_buf[next]));
		if (ret == 0) {
			hook_4g_uart_rx_next_buf = next;
		} else {
			LOG_ERR("4G UART RX next buffer failed: %d", ret);
		}
		break;
	}

	case UART_RX_BUF_RELEASED:
		if ((event->data.rx_buf.buf ==
		     hook_4g_uart_rx_buf[hook_4g_uart_rx_current_buf]) &&
		    (hook_4g_uart_rx_next_buf != HOOK_4G_UART_INVALID_BUF)) {
			hook_4g_uart_rx_current_buf = hook_4g_uart_rx_next_buf;
			hook_4g_uart_rx_next_buf = HOOK_4G_UART_INVALID_BUF;
		}
		break;

	case UART_RX_STOPPED:
		LOG_WRN("4G UART async RX stopped (reason=%d)",
			(int)event->data.rx_stop.reason);
		(void)k_work_reschedule(&hook_4g_uart_rx_restart_work,
					K_MSEC(HOOK_4G_UART_RX_RESTART_DELAY_MS));
		break;

	case UART_RX_DISABLED:
		hook_4g_uart_rx_enabled = false;
		if (!hook_4g_uart_rx_restarting) {
			(void)k_work_reschedule(
				&hook_4g_uart_rx_restart_work,
				K_MSEC(HOOK_4G_UART_RX_RESTART_DELAY_MS));
		}
		break;

	case UART_TX_DONE:
		hook_4g_uart_tx_result = (int32_t)event->data.tx.len;
		k_sem_give(&hook_4g_uart_tx_done);
		break;

	case UART_TX_ABORTED:
		hook_4g_uart_tx_result = -ECANCELED;
		k_sem_give(&hook_4g_uart_tx_done);
		break;

	default:
		break;
	}
}

static void hook_4g_uart_tx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		struct hook_4g_uart_tx_item *item = k_fifo_get(
			&hook_4g_uart_tx_fifo, K_FOREVER);
		int ret;

		k_mutex_lock(&hook_4g_uart_tx_mutex, K_FOREVER);

		if (item->generation !=
		    (uint32_t)atomic_get(&hook_4g_uart_tx_generation)) {
			k_mutex_unlock(&hook_4g_uart_tx_mutex);
			k_mem_slab_free(&hook_4g_uart_tx_slab, item);
			continue;
		}

		k_sem_reset(&hook_4g_uart_tx_done);
		hook_4g_uart_tx_result = -EINPROGRESS;
		ret = uart_tx(hook_4g_uart_dev, item->data, item->len,
			      HOOK_4G_UART_TX_TIMEOUT_US);
		hook_4g_uart_tx_in_flight = (ret == 0);

		k_mutex_unlock(&hook_4g_uart_tx_mutex);

		if (ret == 0) {
			ret = k_sem_take(&hook_4g_uart_tx_done,
					 K_MSEC(HOOK_4G_UART_TX_WAIT_MS));
			if (ret != 0) {
				LOG_ERR("4G UART TX completion timeout");
				k_mutex_lock(&hook_4g_uart_tx_mutex, K_FOREVER);
				if (uart_tx_abort(hook_4g_uart_dev) == 0) {
					(void)k_sem_take(
						&hook_4g_uart_tx_done,
						K_FOREVER);
				}
				k_mutex_unlock(&hook_4g_uart_tx_mutex);
			} else if (hook_4g_uart_tx_result < 0) {
				if (item->generation ==
				    (uint32_t)atomic_get(
					    &hook_4g_uart_tx_generation)) {
					LOG_WRN("4G UART TX aborted (%d)",
						hook_4g_uart_tx_result);
				}
			}
		} else {
			LOG_ERR("4G UART TX start failed: %d", ret);
		}

		k_mutex_lock(&hook_4g_uart_tx_mutex, K_FOREVER);
		hook_4g_uart_tx_in_flight = false;
		k_mutex_unlock(&hook_4g_uart_tx_mutex);

		k_mem_slab_free(&hook_4g_uart_tx_slab, item);
	}
}

int hook_4g_uart_init(hook_4g_uart_rx_cb_t rx_cb)
{
	int ret;

	if (!device_is_ready(hook_4g_uart_dev)) {
		return -ENODEV;
	}

	if (rx_cb == NULL) {
		return -EINVAL;
	}

	if (hook_4g_uart_initialized) {
		return 0;
	}

	hook_4g_uart_rx_cb = rx_cb;
	k_work_init_delayable(&hook_4g_uart_rx_restart_work,
			      hook_4g_uart_rx_restart);

	ret = uart_callback_set(hook_4g_uart_dev, hook_4g_uart_event, NULL);
	if (ret != 0) {
		LOG_ERR("4G UART async callback setup failed: %d", ret);
		return ret;
	}

	k_thread_create(&hook_4g_uart_tx_thread_data, hook_4g_uart_tx_stack,
			K_THREAD_STACK_SIZEOF(hook_4g_uart_tx_stack),
			hook_4g_uart_tx_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	k_thread_name_set(&hook_4g_uart_tx_thread_data, "App4GTx");

	hook_4g_uart_initialized = true;
	ret = hook_4g_uart_enable_rx();
	if (ret != 0) {
		(void)k_work_reschedule(&hook_4g_uart_rx_restart_work,
					K_MSEC(HOOK_4G_UART_RX_RETRY_DELAY_MS));
	}

	return 0;
}

int hook_4g_uart_send(const uint8_t *data, uint16_t len)
{
	void *block;
	struct hook_4g_uart_tx_item *item;
	int ret;

	if (!hook_4g_uart_initialized) {
		return -ENODEV;
	}

	if ((data == NULL) || (len == 0U)) {
		return -EINVAL;
	}

	if (len > HOOK_4G_UART_TX_MAX_FRAME) {
		return -EMSGSIZE;
	}

	k_mutex_lock(&hook_4g_uart_tx_mutex, K_FOREVER);

	ret = k_mem_slab_alloc(&hook_4g_uart_tx_slab, &block, K_NO_WAIT);
	if (ret != 0) {
		k_mutex_unlock(&hook_4g_uart_tx_mutex);
		return -ENOMEM;
	}

	item = block;
	memcpy(item->data, data, len);
	item->len = len;
	item->generation = (uint32_t)atomic_get(&hook_4g_uart_tx_generation);
	k_fifo_put(&hook_4g_uart_tx_fifo, item);

	k_mutex_unlock(&hook_4g_uart_tx_mutex);

	return 0;
}

void hook_4g_uart_flush_tx(void)
{
	void *item;

	if (!hook_4g_uart_initialized) {
		return;
	}

	k_mutex_lock(&hook_4g_uart_tx_mutex, K_FOREVER);
	(void)atomic_inc(&hook_4g_uart_tx_generation);

	if (hook_4g_uart_tx_in_flight) {
		(void)uart_tx_abort(hook_4g_uart_dev);
	}

	while ((item = k_fifo_get(&hook_4g_uart_tx_fifo, K_NO_WAIT)) != NULL) {
		k_mem_slab_free(&hook_4g_uart_tx_slab, item);
	}

	k_mutex_unlock(&hook_4g_uart_tx_mutex);
}
