#include "rfid_uplink.h"
#include "rfid_uplink_protocol.h"

#include "hook_4g.h"
#include "hook_4g_cfg.h"
#include "rfid_sample.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

LOG_MODULE_REGISTER(rfid_uplink, CONFIG_LOG_DEFAULT_LEVEL);

#define RFID_UPLINK_CHANNEL 4U

BUILD_ASSERT(RFID_UPLINK_MAX_FRAME_SIZE <= HOOK_4G_PUBLISH_MAX_PAYLOAD,
	     "RFID A6 frame exceeds one 4G publish payload");

struct rfid_uplink_event {
	uint32_t uid;
	uint8_t rssi;
	uint8_t flags;
};

K_MSGQ_DEFINE(rfid_uplink_event_queue, sizeof(struct rfid_uplink_event),
	      CONFIG_RFID_UPLINK_EVENT_QUEUE_DEPTH, 4);
K_MUTEX_DEFINE(rfid_uplink_status_lock);

static atomic_t rfid_uplink_event_drops;
static atomic_t rfid_uplink_pending_tags;
static struct rfid_uplink_tag rfid_uplink_tags[RFID_UPLINK_MAX_TAGS];
static uint8_t rfid_uplink_tag_count;
static struct rfid_uplink_status rfid_uplink_status;

static void rfid_uplink_observe(const struct zbus_channel *chan)
{
	const struct rfid_sample_diagnostic *sample = zbus_chan_const_msg(chan);
	struct rfid_uplink_event event = {
		.uid = sample->tag.uid,
		.rssi = sample->tag.rssi,
		.flags = sample->tag.battery == RFID_SAMPLE_BATTERY_LEVEL_LOW ?
			 RFID_UPLINK_FLAG_LOW_BATTERY : 0U,
	};

	if (k_msgq_put(&rfid_uplink_event_queue, &event, K_NO_WAIT) != 0) {
		atomic_inc(&rfid_uplink_event_drops);
	}
}

ZBUS_LISTENER_DEFINE(rfid_uplink_listener, rfid_uplink_observe);
ZBUS_CHAN_ADD_OBS(rfid_sample_diagnostic_chan, rfid_uplink_listener, 1);

static void rfid_uplink_collect(const struct rfid_uplink_event *event)
{
	for (uint8_t i = 0U; i < rfid_uplink_tag_count; i++) {
		if (rfid_uplink_tags[i].uid == event->uid) {
			rfid_uplink_tags[i].rssi = event->rssi;
			rfid_uplink_tags[i].flags = event->flags;
			return;
		}
	}

	if (rfid_uplink_tag_count >= RFID_UPLINK_MAX_TAGS) {
		k_mutex_lock(&rfid_uplink_status_lock, K_FOREVER);
		rfid_uplink_status.tag_overflows++;
		k_mutex_unlock(&rfid_uplink_status_lock);
		return;
	}

	rfid_uplink_tags[rfid_uplink_tag_count++] = (struct rfid_uplink_tag) {
		.uid = event->uid,
		.rssi = event->rssi,
		.flags = event->flags,
	};
	atomic_set(&rfid_uplink_pending_tags, rfid_uplink_tag_count);
}

static void rfid_uplink_publish_window(void)
{
	uint8_t frame[RFID_UPLINK_MAX_FRAME_SIZE];
	uint16_t sequence;
	size_t frame_len;
	int rc;

	k_mutex_lock(&rfid_uplink_status_lock, K_FOREVER);
	sequence = rfid_uplink_status.next_sequence;
	k_mutex_unlock(&rfid_uplink_status_lock);

	if (hook_4g_cfg_get_state() != HOOK_4G_CFG_DONE) {
		rc = -EAGAIN;
		goto deferred;
	}

	frame_len = rfid_uplink_encode(sequence, rfid_uplink_tags,
				       rfid_uplink_tag_count, frame, sizeof(frame));
	if (frame_len == 0U) {
		rc = -EMSGSIZE;
		goto failed;
	}

	rc = hook_4g_publish(RFID_UPLINK_CHANNEL, frame, (uint16_t)frame_len);
	if (rc != 0) {
		goto failed;
	}

	k_mutex_lock(&rfid_uplink_status_lock, K_FOREVER);
	rfid_uplink_status.windows_sent++;
	if (rfid_uplink_tag_count == 0U) {
		rfid_uplink_status.empty_windows_sent++;
	}
	rfid_uplink_status.next_sequence++;
	rfid_uplink_status.last_error = 0;
	k_mutex_unlock(&rfid_uplink_status_lock);
	rfid_uplink_tag_count = 0U;
	atomic_set(&rfid_uplink_pending_tags, 0);
	return;

deferred:
	k_mutex_lock(&rfid_uplink_status_lock, K_FOREVER);
	rfid_uplink_status.deferred_windows++;
	rfid_uplink_status.last_error = rc;
	k_mutex_unlock(&rfid_uplink_status_lock);
	return;

failed:
	k_mutex_lock(&rfid_uplink_status_lock, K_FOREVER);
	rfid_uplink_status.send_errors++;
	rfid_uplink_status.last_error = rc;
	k_mutex_unlock(&rfid_uplink_status_lock);
}

void rfid_uplink_get_status(struct rfid_uplink_status *status)
{
	if (status == NULL) {
		return;
	}

	k_mutex_lock(&rfid_uplink_status_lock, K_FOREVER);
	*status = rfid_uplink_status;
	status->pending_tags = (uint8_t)atomic_get(&rfid_uplink_pending_tags);
	status->event_drops = (uint32_t)atomic_get(&rfid_uplink_event_drops);
	k_mutex_unlock(&rfid_uplink_status_lock);
}

static void rfid_uplink_thread(void *arg1, void *arg2, void *arg3)
{
	int64_t deadline = k_uptime_get() + CONFIG_RFID_UPLINK_PERIOD_MS;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		struct rfid_uplink_event event;
		int64_t now = k_uptime_get();
		int64_t remaining = deadline - now;

		if (remaining < 0) {
			remaining = 0;
		}
		if (k_msgq_get(&rfid_uplink_event_queue, &event,
			       K_MSEC((uint32_t)remaining)) == 0) {
			rfid_uplink_collect(&event);
		}

		now = k_uptime_get();
		if (now >= deadline) {
			rfid_uplink_publish_window();
			deadline = now + CONFIG_RFID_UPLINK_PERIOD_MS;
		}
	}
}

K_THREAD_DEFINE(rfid_uplink_tid, CONFIG_RFID_UPLINK_STACK_SIZE,
		rfid_uplink_thread, NULL, NULL, NULL, 11, 0, 0);
