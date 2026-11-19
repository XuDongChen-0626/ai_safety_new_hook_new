#include "rfid_sample.h"
#include "../driver/rfid_sample_uart.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(rfid_sample, LOG_LEVEL_INF);

#define RFID_SAMPLE_UART_NODE DT_ALIAS(rfid_uart)

BUILD_ASSERT(DT_NODE_HAS_STATUS(RFID_SAMPLE_UART_NODE, okay),
	     "RFID sample requires an enabled rfid-uart alias");

ZBUS_CHAN_DEFINE(rfid_sample_diagnostic_chan, struct rfid_sample_diagnostic,
		 NULL, NULL, ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

K_MSGQ_DEFINE(rfid_sample_rx_queue, sizeof(struct rfid_sample_rx_block),
	      CONFIG_RFID_SAMPLE_RX_QUEUE_DEPTH, 4);
K_MUTEX_DEFINE(rfid_sample_status_lock);
K_MUTEX_DEFINE(rfid_sample_command_lock);
K_SEM_DEFINE(rfid_sample_response_ready, 0, 1);

static struct k_spinlock response_lock;
static bool mode_pending;
static uint8_t requested_address;
static uint64_t request_started_ms;
static uint64_t next_mode_request_ms;
static struct rfid_sample_response mode_response;

static struct rfid_sample_uart transport;
static struct rfid_sample_protocol protocol;
static struct rfid_sample_status status_snapshot = {
	.uart_name = DEVICE_DT_NAME(RFID_SAMPLE_UART_NODE),
	.baudrate = DT_PROP(RFID_SAMPLE_UART_NODE, current_speed),
};

struct producer_context {
	uint64_t received_ms;
	uint64_t last_received_ms;
	uint32_t sequence;
	uint32_t generation;
	uint32_t stream_resets;
	uint32_t publish_drops;
};

static void receive_response(const struct rfid_sample_response *response, void *user_data)
{
	const struct producer_context *context = user_data;
	k_spinlock_key_t key = k_spin_lock(&response_lock);
	bool address_matches = requested_address == 0U || requested_address == 0xffU ||
			       response->address == requested_address;

	if (mode_pending && response->command == 0x22U && address_matches &&
	    (response->has_mode || response->device_error != 0U) &&
	    context->received_ms >= request_started_ms) {
		mode_response = *response;
		mode_pending = false;
		k_sem_give(&rfid_sample_response_ready);
	}
	k_spin_unlock(&response_lock, key);
}

static void publish_record(const struct rfid_sample_tag_record *tag, void *user_data)
{
	struct producer_context *context = user_data;

	if (++context->sequence == 0U) {
		++context->sequence;
	}
	context->last_received_ms = context->received_ms;
	struct rfid_sample_diagnostic message = {
		.received_ms = context->received_ms,
		.published_ms = (uint64_t)k_uptime_get(),
		.sequence = context->sequence,
		.stream_resets = context->stream_resets,
		.source_id = 1U,
		.tag = *tag,
	};

	if (zbus_chan_pub(&rfid_sample_diagnostic_chan, &message, K_NO_WAIT) != 0) {
		++context->publish_drops;
	}
}

static void update_snapshot(const struct producer_context *context)
{
	k_mutex_lock(&rfid_sample_status_lock, K_FOREVER);
	rfid_sample_protocol_get_stats(&protocol, &status_snapshot.protocol);
	status_snapshot.stream_resets = context->stream_resets;
	status_snapshot.publish_drops = context->publish_drops;
	status_snapshot.last_received_ms = context->last_received_ms;
	k_mutex_unlock(&rfid_sample_status_lock);
}

void rfid_sample_get_status(struct rfid_sample_status *status)
{
	if (status == NULL) {
		return;
	}
	k_mutex_lock(&rfid_sample_status_lock, K_FOREVER);
	*status = status_snapshot;
	k_mutex_unlock(&rfid_sample_status_lock);
	status->rx_bytes = (uint32_t)atomic_get(&transport.rx_bytes);
	status->rx_dropped_bytes = (uint32_t)atomic_get(&transport.dropped_bytes);
	status->uart_errors = (uint32_t)atomic_get(&transport.errors);
	int error = (int)atomic_get(&transport.fatal_error);

	if (error != 0) {
		status->ready = false;
		status->init_error = error;
	}
}

static int command_ready(void)
{
	struct rfid_sample_status status;

	rfid_sample_get_status(&status);
	return status.ready ? 0 : (status.init_error != 0 ? status.init_error : -ENODEV);
}

int rfid_sample_get_mode(uint8_t address, struct rfid_sample_response *response)
{
	uint8_t request[RFID_SAMPLE_PROTOCOL_MAX_REQUEST_SIZE];
	int rc;

	if (response == NULL || k_is_in_isr()) {
		return -EINVAL;
	}
	memset(response, 0, sizeof(*response));
	if (k_mutex_lock(&rfid_sample_command_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	rc = command_ready();
	if (rc != 0) {
		goto out;
	}
	if ((uint64_t)k_uptime_get() < next_mode_request_ms) {
		rc = -EAGAIN;
		goto out;
	}

	size_t length = rfid_sample_protocol_make_request(0x22U, address,
							 request, sizeof(request));

	if (length == 0U) {
		rc = -EINVAL;
		goto out;
	}
	k_sem_reset(&rfid_sample_response_ready);
	k_spinlock_key_t key = k_spin_lock(&response_lock);

	requested_address = address;
	request_started_ms = (uint64_t)k_uptime_get();
	memset(&mode_response, 0, sizeof(mode_response));
	mode_pending = true;
	k_spin_unlock(&response_lock, key);
	rc = rfid_sample_uart_send(&transport, request, length, K_MSEC(100));
	if (rc == 0 && k_sem_take(&rfid_sample_response_ready, K_MSEC(1000)) != 0) {
		rc = (int)atomic_get(&transport.fatal_error);
		if (rc == 0) {
			rc = -ETIMEDOUT;
		}
	}
	key = k_spin_lock(&response_lock);
	mode_pending = false;
	if (rc == 0) {
		*response = mode_response;
	}
	k_spin_unlock(&response_lock, key);
	if (rc != 0) {
		/* No transaction ID exists. Reject immediate retries after failure;
		 * this drains common late replies, not an absolute stale-reply proof.
		 */
		next_mode_request_ms = (uint64_t)k_uptime_get() + 1000U;
	} else if (response->device_error != 0U) {
		rc = -EIO;
	}
out:
	k_mutex_unlock(&rfid_sample_command_lock);
	return rc;
}

int rfid_sample_inventory(bool start, uint8_t address)
{
	uint8_t request[RFID_SAMPLE_PROTOCOL_MAX_REQUEST_SIZE];
	int rc;

	if (k_is_in_isr()) {
		return -EINVAL;
	}
	if (k_mutex_lock(&rfid_sample_command_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	rc = command_ready();
	if (rc == 0) {
		size_t length = rfid_sample_protocol_make_request(start ? 0x39U : 0x3bU,
							 address, request, sizeof(request));

		rc = length == 0U ? -EINVAL :
			rfid_sample_uart_send(&transport, request, length, K_MSEC(100));
	}
	k_mutex_unlock(&rfid_sample_command_lock);
	return rc;
}

int rfid_sample_set_baudrate(uint32_t baudrate)
{
	int rc;

	if (k_is_in_isr() || (baudrate != 9600U && baudrate != 115200U)) {
		return -EINVAL;
	}
	if (k_mutex_lock(&rfid_sample_command_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	rc = command_ready();
	if (rc == 0) {
		rc = rfid_sample_uart_set_baudrate(&transport, baudrate);
	}
	if (rc == 0) {
		k_mutex_lock(&rfid_sample_status_lock, K_FOREVER);
		status_snapshot.baudrate = baudrate;
		k_mutex_unlock(&rfid_sample_status_lock);
	}
	k_mutex_unlock(&rfid_sample_command_lock);
	return rc;
}

static void receiver_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	struct producer_context context = {0};
	const struct device *uart = DEVICE_DT_GET(RFID_SAMPLE_UART_NODE);

	rfid_sample_protocol_init(&protocol, CONFIG_RFID_SAMPLE_FRAME_TIMEOUT_MS);
	rfid_sample_protocol_set_response_callback(&protocol, receive_response, &context);
	int rc = rfid_sample_uart_start(&transport, uart, &rfid_sample_rx_queue);

	k_mutex_lock(&rfid_sample_status_lock, K_FOREVER);
	status_snapshot.init_error = rc;
	status_snapshot.ready = rc == 0;
	k_mutex_unlock(&rfid_sample_status_lock);
	if (rc != 0) {
		LOG_ERR("RFID UART initialization failed: %d", rc);
		return;
	}
	LOG_INF("RFID diagnostic RX on %s; checksum unchecked; use 'rfid stream on'",
		uart->name);

	while (true) {
		struct rfid_sample_rx_block block;

		rc = k_msgq_get(&rfid_sample_rx_queue, &block, K_MSEC(50));
		if (rc == 0) {
			if (block.generation != context.generation) {
				rfid_sample_protocol_reset(&protocol);
				context.generation = block.generation;
				++context.stream_resets;
			}
			context.received_ms = block.received_ms;
			rfid_sample_protocol_feed(&protocol, block.data, block.length,
					   block.received_ms, publish_record, &context);
		} else {
			/* Use arrival times while draining backlog; wall time only
			 * expires a partial frame once the RX queue is empty.
			 */
			rfid_sample_protocol_expire(&protocol, (uint64_t)k_uptime_get());
		}
		update_snapshot(&context);
		if (atomic_get(&transport.fatal_error) != 0) {
			rfid_sample_protocol_reset(&protocol);
			LOG_ERR("RFID UART stopped: %d",
				(int)atomic_get(&transport.fatal_error));
			return;
		}
	}
}

K_THREAD_DEFINE(rfid_sample_receiver, CONFIG_RFID_SAMPLE_RX_STACK_SIZE,
		receiver_thread, NULL, NULL, NULL, 10, 0, 0);
