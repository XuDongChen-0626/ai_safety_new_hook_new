/*
 * hook_ota.h - HTTP OTA service (4G module downloads into MCUboot slot 1).
 *
 * The cloud asks for a download with "craner#AT+OTADL=<url>"; the module
 * switches to HTTPD mode and pulls the URL itself, the response body is
 * streamed into the MCUboot secondary slot with the Zephyr flash image writer
 * and verified against the Content-Length the server announced. The module is
 * then switched back to MQTT and the controller keeps running the current
 * image.
 *
 * "craner#AT+OTATEST" arms boot_request_upgrade(BOOT_UPGRADE_TEST) and
 * reboots; MCUboot performs the swap and rolls back if the new image is not
 * confirmed in time (ota_manager_service).
 */
#ifndef HOOK_OTA_H
#define HOOK_OTA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_HOOK_OTA_ENABLE)

void hook_ota_init(void);

/*
 * Confirm the running image (engineer driven): clears the MCUboot test state
 * and cancels the "not confirmed -> rollback" timer of ota_manager_service.
 * Returns 0 on success.
 */
int hook_ota_confirm(void);

/*
 * HTTP download session (4G module in HTTPD mode).
 *
 * The module streams the HTTP response head and body over the serial line, so
 * the caller feeds the bytes as they arrive. The head is parsed for
 * Content-Length, which is the exact body size (image + signature TLV), and
 * slot1 is opened as soon as it is known:
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
/* Body size the server announced (0 until the response head is parsed). */
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
const char *hook_ota_http_state_name(uint8_t state);

/*
 * Install one specific image.
 *
 * The hash prefix selects it, with the same rule as the "image test" shell
 * command (the image must be the one in slot1). Kept separate from the
 * download on purpose - the device keeps running (and reachable over MQTT)
 * until the cloud asks for the test, and nothing has to be remembered in
 * between, so it also works after a reset.
 *
 * Two steps, because the caller has to answer before the reboot
 * (CONFIG_HOOK_OTA_REBOOT_DELAY_MS):
 *
 *   hook_ota_arm_test(prefix)     - look the image up, arm the test upgrade
 *   hook_ota_reboot_for_test(prefix) - log, wait, reboot (does not return)
 */
int hook_ota_arm_test(const char *hash_prefix);
void hook_ota_reboot_for_test(const char *hash_prefix);

/* Create the OTA worker thread. */
void hook_ota_start(void);

#else /* !CONFIG_HOOK_OTA_ENABLE */

static inline void hook_ota_init(void)
{
}

static inline void hook_ota_start(void)
{
}

#endif /* CONFIG_HOOK_OTA_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* HOOK_OTA_H */
