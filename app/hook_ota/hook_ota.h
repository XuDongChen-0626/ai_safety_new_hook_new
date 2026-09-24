/*
 * hook_ota.h - MQTT/4G OTA service: Ymodem -> MCUboot slot 1.
 *
 * The service owns the OTA session on the usr.cn "2," channel:
 *   - hook_ota_request()  : arm the transfer, send the Ymodem 'C'
 *   - hook_ota_feed()     : bytes received on the "2," channel
 *   - hook_ota_cancel()   : abort the transfer (CAN CAN) and clean up
 *
 * The received image is streamed into the MCUboot secondary slot with the
 * Zephyr flash image writer, then validated and armed with
 * boot_request_upgrade(BOOT_UPGRADE_TEST); MCUboot performs the swap and the
 * rollback if the new image is not confirmed in time.
 */
#ifndef HOOK_OTA_H
#define HOOK_OTA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	HOOK_OTA_IDLE = 0,
	HOOK_OTA_WAIT_START,   /* armed, waiting for the Ymodem block 0 */
	HOOK_OTA_RECEIVING,    /* image data streaming into slot 1 */
	HOOK_OTA_VERIFYING,    /* image written, header being checked */
	HOOK_OTA_STAGED,       /* slot 1 verified, waiting for the test request */
	HOOK_OTA_ERROR,
} hook_ota_state_t;

#if defined(CONFIG_HOOK_OTA_ENABLE)

void hook_ota_init(void);

/* Arm an OTA session (idempotent while a session is already running). */
int hook_ota_request(void);

/* Abort the session, if any. */
void hook_ota_cancel(void);

/*
 * Confirm the running image (engineer driven): clears the MCUboot test state
 * and cancels the "not confirmed -> rollback" timer of ota_manager_service.
 * Returns 0 on success.
 */
int hook_ota_confirm(void);

/* True while a session is armed or receiving. */
bool hook_ota_is_active(void);

/* Feed bytes received from the OTA channel ("2," prefix stripped). */
void hook_ota_feed(const uint8_t *data, uint16_t len);

/*
 * HTTP download session (4G module in HTTPD mode).
 *
 * The module streams the HTTP response body over the serial line, so the
 * caller feeds the bytes as they arrive. The image size is not known up front
 * (the module filters the response headers, so there is no Content-Length), so
 * the session starts by collecting the MCUboot header and only then asks the
 * flash writer to open slot1:
 *
 *   hook_ota_http_reset()          - arm, called when the download is triggered
 *   hook_ota_http_feed(data, len)  - response body bytes
 *   -> returns 1 when the whole image has been written and slot1 armed
 *
 * Any error leaves the session aborted; the caller must switch the module back
 * to MQTT either way.
 */
void hook_ota_http_reset(void);
int hook_ota_http_feed(const uint8_t *data, uint16_t len);

/*
 * Close the session once the stream has stopped.
 *
 * The transfer must NOT be cut at "header + image_size": an MCUboot signed
 * image carries its signature/hash TLV after the image data (1328 bytes for
 * this project), and truncating it makes MCUboot reject the image with
 * "Image in the secondary slot is not valid!".
 *
 * @return 1 when slot1 was verified and the test upgrade armed, negative on
 *         failure.
 */
int hook_ota_http_finish(void);

/* Uptime of the last received byte (0 when nothing arrived yet). */

/*
 * Arm the HTTP receiver.
 *
 * Only data that arrives after the trigger belongs to the response: the module
 * prints its boot banner ("WH-LTE-7S1") on the same serial line, and feeding
 * that to the image parser produced "not an MCUboot image (magic 0x4C2D4857)".
 */
void hook_ota_http_arm(void);
bool hook_ota_http_is_armed(void);

/* Bytes written into slot1 by the running HTTP session. */
uint32_t hook_ota_http_bytes(void);
/* Expected total size once the MCUboot header has been parsed (0 = unknown). */
uint32_t hook_ota_http_expect_bytes(void);

/*
 * Ask for one HTTP firmware download. url must be a plain HTTP URL
 * ("http://host[:port]/path" or "host[:port]/path"), at most 200 bytes.
 *
 * The request is queued and executed by the OTA worker thread: it switches the
 * 4G module to HTTPD, triggers the GET, streams the image into slot1, and
 * always switches the module back to MQTT when it is over.
 *
 * @return 0 when the request was accepted, -EINVAL on a malformed URL,
 *         -EBUSY when a download is already running.
 */
int hook_ota_http_request(const char *url, uint32_t len);

/* Download progress for the cloud status query. */
enum hook_ota_http_state {
	HOOK_OTA_HTTP_IDLE = 0,
	HOOK_OTA_HTTP_DOWNLOADING = 1,
	HOOK_OTA_HTTP_STAGED = 2,
	HOOK_OTA_HTTP_ERROR = 3,
};
uint8_t hook_ota_http_status(void);

/*
 * Arm the MCUboot test upgrade for the image staged by the HTTP download and
 * reboot.
 *
 * Kept separate from the download on purpose: the device keeps running (and
 * staying reachable over MQTT) until the cloud explicitly asks for the test.
 *
 * @return 0 on success (the reboot happens at the end), -EINVAL when nothing
 *         is staged, -EIO when MCUboot refused to arm the upgrade.
 */
int hook_ota_test_and_reboot(void);

/*
 * True when there is an image ready to be installed: staged earlier in this
 * boot session, or staged by a download that finished before the last reset
 * (the persisted ota.staged marker, which is checked against the image that
 * slot1 actually holds).
 */
bool hook_ota_test_available(void);

/*
 * True when the persisted marker says slot1 holds a verified image that is
 * waiting for AT+OTATEST. Used by AT+OTAINFO and by the boot time notice; the
 * marker is written when a download is verified and cleared when slot1 is
 * overwritten, when the session is cancelled and after a confirmed install.
 */
bool hook_ota_staged_pending(void);

/* Create the OTA worker thread. */
void hook_ota_start(void);

hook_ota_state_t hook_ota_get_state(void);
const char *hook_ota_state_name(hook_ota_state_t state);
uint32_t hook_ota_bytes_received(void);
uint32_t hook_ota_image_size(void);

#else /* !CONFIG_HOOK_OTA_ENABLE */

static inline void hook_ota_init(void)
{
}

static inline int hook_ota_request(void)
{
	return -1;
}

static inline void hook_ota_cancel(void)
{
}

static inline bool hook_ota_is_active(void)
{
	return false;
}

static inline void hook_ota_feed(const uint8_t *data, uint16_t len)
{
	(void)data;
	(void)len;
}

static inline void hook_ota_start(void)
{
}

static inline hook_ota_state_t hook_ota_get_state(void)
{
	return HOOK_OTA_IDLE;
}

static inline const char *hook_ota_state_name(hook_ota_state_t state)
{
	(void)state;
	return "disabled";
}

static inline uint32_t hook_ota_bytes_received(void)
{
	return 0U;
}

static inline uint32_t hook_ota_image_size(void)
{
	return 0U;
}

static inline bool hook_ota_test_available(void)
{
	return false;
}

static inline bool hook_ota_staged_pending(void)
{
	return false;
}

#endif /* CONFIG_HOOK_OTA_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* HOOK_OTA_H */
