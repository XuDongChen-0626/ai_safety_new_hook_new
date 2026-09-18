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
	HOOK_OTA_STAGED,       /* slot 1 armed, reboot pending */
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

#endif /* CONFIG_HOOK_OTA_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* HOOK_OTA_H */
