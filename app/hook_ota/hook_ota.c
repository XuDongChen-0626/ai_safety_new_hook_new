/*
 * hook_ota.c - HTTP OTA service (4G module downloads into MCUboot slot 1).
 *
 * Data flow:
 *   cloud --MQTT topic 1--> "craner#AT+OTADL=<url>"
 *   --> 4G module switches to HTTPD and downloads the URL itself
 *   --> usart1 --> hook_4g --> hook_ota_http_feed() --> flash_img (slot 1)
 *   --> module switches back to MQTT, image verified and staged
 *   --> "craner#AT+OTATEST" arms the MCUboot test upgrade and reboots
 *
 * There is no Ymodem/MQTT firmware transfer any more: it fought over the same
 * module, the same 4G stack and the same slot1 bookkeeping as the HTTP path,
 * and the two could not be told apart reliably. All firmware operations live
 * on the craner#AT command surface.
 *
 * The download result goes back on usr.cn channel 2, which the module routes
 * to ai_safety/<dev>/ota/hook/server.
 */
#include "hook_ota.h"

#include <string.h>
#include <stdlib.h>

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "hook_health.h"
#include "hook_4g.h"
#include "hook_4g_cfg.h"
#include "hook_heartbeat.h"
#include "hook_register.h"
#include "hook_register_map.h"
#include "offline_log_service.h"
#include "osal_zephyr.h"
#include "ota_manager_service.h"
#include "system_def.h"
#include "uart_manage.h"

LOG_MODULE_REGISTER(hook_ota, CONFIG_LOG_DEFAULT_LEVEL);

static struct flash_img_context hook_ota_flash_ctx;
static bool hook_ota_flash_active;
/* Single OTA session state (see enum hook_ota_http_state). */
static volatile uint8_t hook_ota_http_state_value;

/*
 * OTA result channel (usr.cn channel 2 -> ai_safety/<dev>/ota/hook/server).
 * It is publish only: the cloud never sends firmware data over MQTT any more,
 * the module downloads it over HTTP instead.
 */
static int hook_ota_channel_send(const uint8_t *data, uint16_t len)
{
	/*
	 * hook_4g_send_ota() adds the "2," prefix in the same usr.cn frame and
	 * serialises concurrent writes to the 4G link.
	 */
	return hook_4g_send_ota(data, len);
}



/*
 * ---------------------------------------------------------------------------
 * HTTP download session (4G module in HTTPD mode)
 * ---------------------------------------------------------------------------
 *
 * The module streams the HTTP response head and body over the serial line
 * (AT+HTPPK=OFF keeps the head). The head carries Content-Length, which is the
 * exact body size - including the MCUboot signature TLV that follows the
 * image, which is not a fixed size and cannot be derived from the image
 * header. Slot1 is opened as soon as the size is known.
 */
/* Response head buffer (this server sends ~700 B with its CORS headers). */
#define HOOK_OTA_HTTP_HEAD_MAX 1024U
/* Response head, parsed once for the status code and Content-Length. */
static uint8_t hook_ota_http_head[HOOK_OTA_HTTP_HEAD_MAX];
static uint16_t hook_ota_http_head_len;
static bool hook_ota_http_head_done;
/* Body length as declared by the server (Content-Length). */
static uint32_t hook_ota_http_expected;
static uint32_t hook_ota_http_received;
static volatile bool hook_ota_http_armed;

void hook_ota_http_reset(void)
{
	hook_ota_http_head_len = 0U;
	hook_ota_http_head_done = false;
	hook_ota_http_expected = 0U;
	hook_ota_http_received = 0U;
	hook_ota_http_armed = false;
	hook_ota_flash_active = false;
}

void hook_ota_http_arm(void)
{
	hook_ota_http_armed = true;
}

bool hook_ota_http_is_armed(void)
{
	return hook_ota_http_armed;
}

uint32_t hook_ota_http_bytes(void)
{
	return hook_ota_http_received;
}

uint32_t hook_ota_http_expect_bytes(void)
{
	return hook_ota_http_expected;
}

/*
 * Parse the response head: require 200, take Content-Length as the exact body
 * size and refuse anything the downloader cannot handle.
 */
static int hook_ota_http_parse_head(const char *head, uint32_t *body_len)
{
	const char *p;
	const char *cl;
	long status;
	unsigned long len;

	if (strncmp(head, "HTTP/", 5U) != 0) {
		LOG_ERR("HTTP OTA: response does not start with a status line");
		return -EBADMSG;
	}

	p = strchr(head, ' ');
	if (p == NULL) {
		return -EBADMSG;
	}

	status = strtol(p + 1, NULL, 10);
	if (status != 200) {
		LOG_ERR("HTTP OTA: server answered %ld, expected 200", status);
		return -EIO;
	}

	if (strstr(head, "chunked") != NULL) {
		LOG_ERR("HTTP OTA: chunked responses are not supported");
		return -ENOTSUP;
	}

	cl = strstr(head, "Content-Length:");
	if (cl == NULL) {
		LOG_ERR("HTTP OTA: no Content-Length in the response");
		return -EBADMSG;
	}

	len = strtoul(cl + strlen("Content-Length:"), NULL, 10);
	if (len == 0UL) {
		LOG_ERR("HTTP OTA: empty body advertised");
		return -EBADMSG;
	}

	*body_len = (uint32_t)len;

	return 0;
}

/* Open slot1 for a body of the given length. */
static int hook_ota_http_open_slot(uint32_t body_len)
{
	uint32_t slot_size = PARTITION_SIZE(slot1_partition);
	int ret;

	if ((body_len == 0U) || (body_len > slot_size)) {
		LOG_ERR("HTTP OTA: body of %u B does not fit slot1 (%u B)",
			body_len, slot_size);
		return -EINVAL;
	}

	ret = flash_img_init(&hook_ota_flash_ctx);
	if (ret != 0) {
		LOG_ERR("HTTP OTA: flash_img_init failed: %d", ret);
		return -EIO;
	}

	hook_ota_flash_active = true;
	hook_ota_http_expected = body_len;

	LOG_INF("HTTP OTA: slot1 open, expecting %u B (from Content-Length)",
		body_len);
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA: response head accepted, body %u B",
				body_len);

	return 0;
}

/* Flush slot1, verify the image and arm the MCUboot test upgrade. */
int hook_ota_http_finish(void)
{
	struct mcuboot_img_header header;
	uint32_t written;
	int ret;

	/* The response is complete: stop feeding the body into the session. */
	hook_ota_http_armed = false;

	ret = flash_img_buffered_write(&hook_ota_flash_ctx, NULL, 0U, true);
	hook_ota_flash_active = false;
	if (ret != 0) {
		LOG_ERR("HTTP OTA: flash flush failed: %d", ret);
		goto error;
	}

	written = (uint32_t)flash_img_bytes_written(&hook_ota_flash_ctx);
	if (written < hook_ota_http_expected) {
		LOG_ERR("HTTP OTA: short write %u of %u B", written,
			hook_ota_http_expected);
		goto error;
	}

	memset(&header, 0, sizeof(header));
	ret = boot_read_bank_header(PARTITION_ID(slot1_partition), &header,
				    sizeof(header));
	if (ret != 0) {
		LOG_ERR("HTTP OTA: slot1 does not hold a valid MCUboot image: %d",
			ret);
		goto error;
	}

	LOG_INF("HTTP OTA: slot1 image valid: v%u.%u.%u img=%u B",
		header.h.v1.sem_ver.major, header.h.v1.sem_ver.minor,
		header.h.v1.sem_ver.revision, header.h.v1.image_size);

	hook_health_ota_gate(false);

	/*
	 * Deliberately NOT boot_request_upgrade() here: the device stays on the
	 * running image (and therefore reachable over MQTT) until the cloud asks
	 * for the test by hash, see hook_ota_test_and_reboot().
	 */
	LOG_INF("HTTP OTA ready: slot1 image v%u.%u.%u (%u B) verified, "
		"waiting for the test request",
		header.h.v1.sem_ver.major, header.h.v1.sem_ver.minor,
		header.h.v1.sem_ver.revision, header.h.v1.image_size);
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA ready: slot1 image v%u.%u.%u (%u B) verified, "
				"waiting for the test request",
				header.h.v1.sem_ver.major,
				header.h.v1.sem_ver.minor,
				header.h.v1.sem_ver.revision,
				header.h.v1.image_size);
	return 1;

error:
	hook_ota_http_state_value = HOOK_OTA_HTTP_ERROR;
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
				"HTTP OTA failed after %u B, staying on the running image",
				hook_ota_http_received);
	return -EIO;
}

int hook_ota_http_feed(const uint8_t *data, uint16_t len)
{
	uint16_t copy;
	int ret;

	if ((data == NULL) || (len == 0U)) {
		return 0;
	}

	/*
	 * Phase 0: response head. It carries Content-Length, which is the only
	 * exact statement about the body size - the MCUboot signature TLV that
	 * follows the image data is not a fixed size, so it cannot be derived
	 * from the image header.
	 */
	if (!hook_ota_http_head_done) {
		uint16_t space = (uint16_t)(sizeof(hook_ota_http_head) - 1U -
					    hook_ota_http_head_len);
		uint16_t body_start = 0U;

		copy = (len < space) ? len : space;
		if (copy > 0U) {
			memcpy(&hook_ota_http_head[hook_ota_http_head_len], data,
			       copy);
			hook_ota_http_head_len =
				(uint16_t)(hook_ota_http_head_len + copy);
			data += copy;
			len = (uint16_t)(len - copy);
		}

		/* The head ends with an empty line. */
		for (uint16_t i = 3U; i < hook_ota_http_head_len; i++) {
			if ((hook_ota_http_head[i - 3U] == '\r') &&
			    (hook_ota_http_head[i - 2U] == '\n') &&
			    (hook_ota_http_head[i - 1U] == '\r') &&
			    (hook_ota_http_head[i] == '\n')) {
				body_start = (uint16_t)(i + 1U);
				break;
			}
		}

		if (body_start == 0U) {
			if (hook_ota_http_head_len >=
			    (sizeof(hook_ota_http_head) - 1U)) {
				LOG_ERR("HTTP OTA: response head too long");
				goto fail;
			}
			return 0;
		}

		hook_ota_http_head[hook_ota_http_head_len] = '\0';

		ret = hook_ota_http_parse_head((const char *)hook_ota_http_head,
					       &hook_ota_http_expected);
		if (ret != 0) {
			goto fail;
		}

		ret = hook_ota_http_open_slot(hook_ota_http_expected);
		if (ret != 0) {
			goto fail;
		}

		hook_ota_http_head_done = true;

		/* The body may already have started inside the same chunk. */
		if (hook_ota_http_head_len > body_start) {
			uint16_t extra =
				(uint16_t)(hook_ota_http_head_len - body_start);

			ret = flash_img_buffered_write(
				&hook_ota_flash_ctx,
				&hook_ota_http_head[body_start], extra, false);
			if (ret != 0) {
				LOG_ERR("HTTP OTA: flash write failed: %d", ret);
				goto fail;
			}
			hook_ota_http_received += extra;
		}

		if (len == 0U) {
			return (hook_ota_http_received >=
				hook_ota_http_expected) ? 1 : 0;
		}
	}

	/* Phase 1: the body itself (image + signature TLV). */
	if (hook_ota_http_received < hook_ota_http_expected) {
		uint32_t remaining =
			hook_ota_http_expected - hook_ota_http_received;

		if ((uint32_t)len > remaining) {
			len = (uint16_t)remaining;
		}

		ret = flash_img_buffered_write(&hook_ota_flash_ctx, data, len,
					       false);
		if (ret != 0) {
			LOG_ERR("HTTP OTA: flash write failed at %u: %d",
				hook_ota_http_received, ret);
			goto fail;
		}

		hook_ota_http_received += len;
	}

	return (hook_ota_http_received >= hook_ota_http_expected) ? 1 : 0;

fail:
	hook_ota_http_reset();
	hook_ota_http_state_value = HOOK_OTA_HTTP_ERROR;
	return -EIO;
}

/*
 * ---------------------------------------------------------------------------
 * HTTP OTA orchestration
 * ---------------------------------------------------------------------------
 *
 * Triggered by "craner#AT+OTADL=<url>" (topic 1). The
 * whole sequence takes tens of seconds, so it runs on its own thread:
 *
 *   mute business TX -> switch the module to HTTPD -> trigger the GET
 *   -> stream the image into slot1 -> switch the module back to MQTT -> unmute
 *
 * The "switch back" step runs on every path, failures included: leaving the
 * module in HTTPD mode would cost the device its MQTT link.
 */
#define HOOK_OTA_HTTP_URL_MAX  208U
#define HOOK_OTA_HTTP_HOST_MAX 64U
#define HOOK_OTA_HTTP_PATH_MAX 200U
/* Upper bound for one download (280 KB at serial line speed takes ~25 s). */
#define HOOK_OTA_HTTP_MAX_MS (10U * 60U * 1000U)
/* Module side HTTP timeout (AT+HTPTIM). */
#define HOOK_OTA_HTTP_MODEM_TIMEOUT_S 120U
/*
 * How long one trigger is given before it is sent again.
 *
 * Measured on the bench: the response started ~26 s after the trigger, so a
 * short window made the flow re-trigger while the first request was still on
 * its way - three triggers could then be concatenated into one garbage
 * request. Fewer triggers with a longer window work better.
 */
#define HOOK_OTA_HTTP_TRIGGER_WAIT_MS 40000U
#define HOOK_OTA_HTTP_TRIGGER_TRIES   2U
/*
 * Guard between queueing the download and muting the business TX path: the AT
 * handler that requested it (craner#AT+OTADL) publishes its "craner#OK" as soon
 * as hook_ota_http_request() returns, so the mute must not come first.
 */
#define HOOK_OTA_HTTP_TX_SETTLE_MS    300U

static char hook_ota_http_url[HOOK_OTA_HTTP_URL_MAX];
static volatile bool hook_ota_http_requested;
K_SEM_DEFINE(hook_ota_http_signal, 0, 1);

/*
 * Publish the download result on the OTA topic (channel 2 ->
 * ai_safety/<dev>/ota/hook/server).
 *
 * The module is back in MQTT mode by then, and the cloud needs this to decide
 * whether to send AT+OTATEST. The payload stays short (hook_4g_send_ota() takes
 * at most 62 bytes) and uses the "+..." style of the AT answers:
 *
 *   +OTADL:OK,<body bytes>,<image bytes>,v<version>,<hash prefix>
 *   +OTADL:ERR,<reason>,<body bytes>,<expected bytes>
 *
 * reason: 1 URL rejected, 2 module setup failed, 3 no response data,
 *         4 receive/write/verify failed.
 */
static void hook_ota_publish_result(uint8_t reason)
{
	struct ota_manager_image_info info;
	char line[64];
	int written;

	if (reason == 0U) {
		if ((ota_manager_get_image_info(1U, &info) == 0) && info.valid) {
			written = snprintf(line, sizeof(line), "+OTADL:OK,%u,%u,%s,%.8s",
					   hook_ota_http_bytes(), info.image_size,
					   info.version, info.hash_hex);
		} else {
			written = snprintf(line, sizeof(line), "+OTADL:OK,%u,0,-,-",
					   hook_ota_http_bytes());
		}
	} else {
		written = snprintf(line, sizeof(line), "+OTADL:ERR,%u,%u,%u",
				   (unsigned int)reason, hook_ota_http_bytes(),
				   hook_ota_http_expect_bytes());
	}

	if ((written <= 0) || ((size_t)written >= sizeof(line))) {
		LOG_ERR("OTA result message too long");
		return;
	}

	/*
	 * Publish ONCE, on channel 2 (ai_safety/<dev>/ota/hook/server) - the
	 * topic that exists for exactly this: OTA results.
	 *
	 * One channel only, on purpose: the module merges serial frames that
	 * follow each other without a gap (20 ms packet timeout) into ONE MQTT
	 * message and honours only the first channel prefix. Publishing the
	 * same result on channel 1 and channel 2 back to back made the cloud
	 * receive
	 *
	 *   craner#+OTADL:OK,252256,250896,2.1.0+0,7539b8142,+OTADL:OK,...
	 *
	 * i.e. both payloads glued together with a stray "2," inside (seen
	 * 2026-09-24 18:01).
	 *
	 * The cloud has to subscribe to this topic to see the result; the
	 * command answers stay on channel 1 as before.
	 */
	if (hook_ota_channel_send((const uint8_t *)line, (uint16_t)written) != 0) {
		LOG_WRN("OTA result not published on the OTA topic: %s", line);
		return;
	}

	LOG_INF("OTA result published on the OTA topic: %s", line);
}

uint8_t hook_ota_http_status(void)
{
	return hook_ota_http_state_value;
}

const char *hook_ota_http_state_name(uint8_t state)
{
	switch (state) {
	case HOOK_OTA_HTTP_IDLE:
		return "idle";
	case HOOK_OTA_HTTP_DOWNLOADING:
		return "downloading";
	case HOOK_OTA_HTTP_STAGED:
		return "staged";
	case HOOK_OTA_HTTP_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

int hook_ota_http_request(const char *url, uint32_t len)
{
	if ((url == NULL) || (len == 0U) || (len >= sizeof(hook_ota_http_url))) {
		return -EINVAL;
	}

	if (hook_ota_http_requested ||
	    (hook_ota_http_state_value == HOOK_OTA_HTTP_DOWNLOADING)) {
		return -EBUSY;
	}

	memcpy(hook_ota_http_url, url, len);
	hook_ota_http_url[len] = '\0';
	hook_ota_http_requested = true;
	k_sem_give(&hook_ota_http_signal);

	return 0;
}

/* Split "http://host[:port]/path"; the scheme is optional. */
static int hook_ota_http_parse(const char *url, char *host, size_t host_len,
			       uint16_t *port, char *path, size_t path_len)
{
	const char *p = url;
	const char *slash;
	const char *colon;
	size_t used;

	if (strncmp(p, "http://", 7U) == 0) {
		p += 7U;
	}

	slash = strchr(p, '/');
	if (slash == NULL) {
		return -EINVAL;
	}

	colon = memchr(p, ':', (size_t)(slash - p));
	used = (size_t)((colon != NULL) ? (colon - p) : (slash - p));

	if ((used == 0U) || (used >= host_len)) {
		return -EINVAL;
	}

	memcpy(host, p, used);
	host[used] = '\0';

	*port = 80U;
	if (colon != NULL) {
		long value = strtol(colon + 1, NULL, 10);

		if ((value <= 0) || (value > 65535)) {
			return -EINVAL;
		}
		*port = (uint16_t)value;
	}

	if (strlen(slash) >= path_len) {
		return -EINVAL;
	}
	strcpy(path, slash);

	return 0;
}

static void hook_ota_http_run(void)
{
	char host[HOOK_OTA_HTTP_HOST_MAX];
	char path[HOOK_OTA_HTTP_PATH_MAX];
	bool staged = false;
	uint16_t port = 80U;
	uint32_t start;
	uint8_t fail_reason = 0U;
	int ret;

	hook_ota_http_requested = false;

	ret = hook_ota_http_parse(hook_ota_http_url, host, sizeof(host), &port,
				  path, sizeof(path));
	if (ret != 0) {
		hook_ota_http_state_value = HOOK_OTA_HTTP_ERROR;
		LOG_ERR("HTTP OTA: malformed URL");
		(void)offline_log_event(OFFLINE_LOG_CAT_OTA,
					OFFLINE_LOG_LEVEL_ERR,
					"HTTP OTA rejected: malformed URL");
		hook_ota_publish_result(1U);
		return;
	}

	LOG_INF("HTTP OTA: downloading %s:%u%s", host, (unsigned int)port, path);
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA start: %s:%u%s", host,
				(unsigned int)port, path);

	hook_ota_http_state_value = HOOK_OTA_HTTP_DOWNLOADING;
	hook_ota_http_reset();

	/* Let the "craner#OK" of the requesting command reach the module. */
	k_msleep(HOOK_OTA_HTTP_TX_SETTLE_MS);

	/* Mute business TX first: in HTTPD mode anything sent to the module is
	 * appended to the HTTP request being built. */
	hook_4g_set_http_mode(true);
	hook_heartbeat_set_suspend(true);

	/*
	 * Stop the task check-in supervision for the whole session: the 4G
	 * thread blocks for seconds inside the slot1 sector erases, and the
	 * module restarts two times on the way in and out. Pausing the
	 * supervision is the module-level way to do that, see system_def.h.
	 */
	hook_health_ota_gate(true);

	ret = hook_4g_cfg_enter_httpd(host, port, path,
				      HOOK_OTA_HTTP_MODEM_TIMEOUT_S);

	if (ret == 0) {
		/*
		 * The module only forwards the trigger to the server once it is
		 * online, and it silently drops it otherwise, so retry the
		 * request until data starts arriving.
		 */
		for (uint32_t attempt = 1U;
		     attempt <= HOOK_OTA_HTTP_TRIGGER_TRIES; attempt++) {
			if (hook_ota_http_bytes() > 0U) {
				break;
			}

			LOG_INF("HTTP OTA: sending GET (attempt %u)", attempt);
			ret = hook_4g_http_trigger(host, port);
			if (ret != 0) {
				break;
			}
			/* From here on the response body may arrive. */
			hook_ota_http_arm();

			start = k_uptime_get_32();
			while ((uint32_t)(k_uptime_get_32() - start) <
			       HOOK_OTA_HTTP_TRIGGER_WAIT_MS) {
				if (hook_ota_http_bytes() > 0U) {
					break;
				}
				k_msleep(200);
			}
		}

		if (hook_ota_http_bytes() == 0U) {
			LOG_ERR("HTTP OTA: no data after %u trigger(s)",
				(unsigned int)HOOK_OTA_HTTP_TRIGGER_TRIES);
			fail_reason = 3U;
		} else {
			/*
			 * Wait for the whole body: Content-Length gives the exact
			 * size, so the session ends as soon as that many bytes
			 * have been written.
			 */
			start = k_uptime_get_32();
			while ((uint32_t)(k_uptime_get_32() - start) <
			       HOOK_OTA_HTTP_MAX_MS) {
				uint32_t expect = hook_ota_http_expect_bytes();

				/* The receiver gave up (write error, short
				 * write, image not valid). */
				if (hook_ota_http_state_value ==
				    HOOK_OTA_HTTP_ERROR) {
					break;
				}

				/*
				 * Content-Length is exact, so the body is complete
				 * the moment that many bytes have been written.
				 */
				if ((expect > 0U) &&
				    (hook_ota_http_bytes() >= expect)) {
					LOG_INF("HTTP OTA: body complete (%u B)",
						hook_ota_http_bytes());
					staged = (hook_ota_http_finish() == 1);
					break;
				}

				k_msleep(100);
			}
		}
	} else {
		LOG_ERR("HTTP OTA: module setup failed: %d", ret);
		fail_reason = 2U;
	}

	/*
	 * Always give the MQTT link back. This must not be "best effort": an
	 * unreachable device has lost its only management channel, so a second
	 * failure reboots the controller - the boot time provisioning flow puts
	 * the module back into MQTT mode whenever it is not in it.
	 */
	ret = hook_4g_cfg_enter_mqtt();

	/*
	 * -ENOTCONN is different: the module is in MQTT mode with a verified
	 * configuration but has no session, which is a network or server problem.
	 * A controller reboot would not reset the module, would not help the
	 * network, and would throw away the image that is already staged - so
	 * keep running and report it (the module retries on its own).
	 */
	if (ret == -ENOTCONN) {
		LOG_ERR("HTTP OTA: MQTT session did not come up, staged image kept");
		(void)offline_log_event(
			OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
			"HTTP OTA: module back in MQTT mode without a session, staged image kept");
	} else if (ret != 0) {
		LOG_ERR("HTTP OTA: MQTT restore failed (%d), retrying", ret);
		k_msleep(2000);
		ret = hook_4g_cfg_enter_mqtt();

		if (ret != 0) {
			LOG_ERR("HTTP OTA: MQTT restore failed twice, rebooting to recover");
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
				"HTTP OTA: MQTT restore failed twice, rebooting to recover");
			hook_4g_set_http_mode(false);
			k_msleep(200);
			sys_reboot(SYS_REBOOT_COLD);
		}
	}

	hook_4g_set_http_mode(false);
	hook_heartbeat_set_suspend(false);
	hook_health_ota_gate(false);

	if (staged) {
		hook_ota_http_state_value = HOOK_OTA_HTTP_STAGED;

		/*
		 * The image is staged either way and stays usable; only the MQTT
		 * session may still be pending, so say which one it is.
		 */
		if (ret == -ENOTCONN) {
			LOG_WRN("HTTP OTA: complete, %u B written to slot1, "
				"MQTT session not up yet",
				hook_ota_http_bytes());
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
				"HTTP OTA ready, waiting for the test request (MQTT session pending)");
		} else {
			LOG_INF("HTTP OTA: complete, %u B written to slot1, MQTT restored",
				hook_ota_http_bytes());
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"HTTP OTA ready, MQTT restored, waiting for the test request");
		}

		/* Tell the cloud it can ask for the test. */
		hook_ota_publish_result(0U);
		return;
	}

	hook_ota_http_state_value = HOOK_OTA_HTTP_ERROR;
	LOG_ERR("HTTP OTA: failed (state=%s, %u/%u B)",
		hook_ota_http_state_name(hook_ota_http_state_value),
		hook_ota_http_bytes(), hook_ota_http_expect_bytes());
	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
				"HTTP OTA failed (%u/%u B), MQTT restored",
				hook_ota_http_bytes(),
				hook_ota_http_expect_bytes());

	/*
	 * 4 = receive/write/verify failed; 2/3 were set by the step that failed.
	 */
	hook_ota_publish_result((fail_reason != 0U) ? fail_reason : 4U);
}

static void hook_ota_http_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		(void)k_sem_take(&hook_ota_http_signal, K_FOREVER);
		hook_ota_http_run();
	}
}

K_THREAD_STACK_DEFINE(hook_ota_http_stack, 4096);
static struct k_thread hook_ota_http_thread_obj;

void hook_ota_init(void)
{
	hook_ota_http_state_value = HOOK_OTA_HTTP_IDLE;

	/*
	 * Report what slot1 holds: after a download (or a rollback) an image can
	 * sit there without anything in RAM knowing about it, and the hash is
	 * what selects it for "craner#AT+OTATEST=<hash>".
	 */
	{
		struct ota_manager_image_info slot1;

		if ((ota_manager_get_image_info(1U, &slot1) == 0) && slot1.valid) {
			LOG_INF("slot1 image: v%s %.8s (install with "
				"craner#AT+OTATEST=%.8s)",
				slot1.version, slot1.hash_hex, slot1.hash_hex);
			(void)offline_log_event(
				OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"slot1 image v%s %.8s, install with AT+OTATEST=%.8s",
				slot1.version, slot1.hash_hex, slot1.hash_hex);
		}
	}
}

int hook_ota_confirm(void)
{
	int ret = ota_manager_confirm_current_image();

	if (ret != 0) {
		(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_ERR,
					"image confirm failed (%d)", ret);
		return ret;
	}

	(void)offline_log_event(OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_INF,
				"running image confirmed by the engineer");

	return 0;
}

/*
 * Arm the MCUboot test upgrade for one specific image.
 *
 * The hash prefix selects it - the same rule as the "image test" shell command
 * (ota_manager_request_image_test()): the image must be the one in slot1, so
 * the running image can never be selected, and after a swap the old image that
 * ended up in slot1 is only installed if its hash is asked for explicitly.
 * Nothing has to be remembered between the download and the test, which is why
 * this needs no persisted state and also works after a reset.
 *
 * @return 0 when the upgrade is armed, or the negative error of the lookup
 *         (image not found, ambiguous prefix, or the match is not in slot1).
 */
int hook_ota_arm_test(const char *hash_prefix)
{
	int ret;

	if ((hash_prefix == NULL) || (hash_prefix[0] == '\0')) {
		return -EINVAL;
	}

	ret = ota_manager_request_image_test(hash_prefix);
	if (ret != 0) {
		LOG_WRN("OTA test rejected for image %s: %d", hash_prefix, ret);
		(void)offline_log_event(
			OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
			"OTA test rejected for image %s (%d)", hash_prefix, ret);
		return ret;
	}

	return 0;
}

/*
 * Announce the test, give the answer time to leave the device and reboot:
 * MCUboot swaps slot0/slot1 on the next reset and rolls back unless the new
 * image is confirmed within the window of ota_manager_service.
 *
 * Never returns.
 */
void hook_ota_reboot_for_test(const char *hash_prefix)
{
	LOG_WRN("OTA test requested for image %s: rebooting in %u ms for MCUboot",
		hash_prefix,
		(unsigned int)CONFIG_HOOK_OTA_REBOOT_DELAY_MS);
	(void)offline_log_event(
		OFFLINE_LOG_CAT_OTA, OFFLINE_LOG_LEVEL_WRN,
		"OTA test requested for image %s, rebooting into the MCUboot test image",
		hash_prefix);

	k_msleep(CONFIG_HOOK_OTA_REBOOT_DELAY_MS);
	sys_reboot(SYS_REBOOT_COLD);
}

void hook_ota_start(void)
{
	k_thread_create(&hook_ota_http_thread_obj, hook_ota_http_stack,
			K_THREAD_STACK_SIZEOF(hook_ota_http_stack),
			hook_ota_http_thread, NULL, NULL, NULL,
			OSAL_PRIO_BELOW_NORMAL, 0, K_NO_WAIT);
	k_thread_name_set(&hook_ota_http_thread_obj, "OtaHttp");
}
