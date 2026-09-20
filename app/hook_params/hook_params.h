/*
 * hook_params.h - product parameter table of the hook controller.
 *
 * The values live in module/device_param_server and are persisted in the
 * QSPI param-store partition (Zephyr Settings/ZMS), exactly like the identity
 * labels registered by module/device_identity_service:
 *
 *   param list                      - show every parameter
 *   param set device.no ais011      - set the factory device number
 *   param save                      - write it to the QSPI flash
 *
 * The factory device number is the identifier the cloud uses in the MQTT
 * topics (e.g. "ais011"); when it is empty the hardware derived short id from
 * the identity service is used instead, so a device is never anonymous.
 */
#ifndef HOOK_PARAMS_H
#define HOOK_PARAMS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the product parameter table (called from SYS_INIT). */
int hook_params_init(void);

/* Re-read the parameters from the parameter server (after "param set"). */
void hook_params_refresh(void);

/*
 * Write the whole parameter list to the offline log (SD card), one line per
 * parameter, e.g. `param device.no="ais011" (boot)`.
 *
 * reason is a short tag shown in every line ("boot", "saved", ...). It is
 * called once at boot and again whenever the parameter server reports that a
 * "param save" actually wrote something to the QSPI param-store.
 */
#if defined(CONFIG_OFFLINE_LOG_SERVICE)
void hook_params_log_all(const char *reason);
#else
static inline void hook_params_log_all(const char *reason)
{
	(void)reason;
}
#endif

/* Factory device number; defaults to the hardware derived short id. */
const char *hook_params_device_no(void);

/* Device tag used in logs / MQTT topics: always device.no. */
const char *hook_params_device_tag(void);

/* MQTT broker settings used by the automatic 4G provisioning. */
const char *hook_params_mqtt_server(void);
const char *hook_params_mqtt_port(void);
const char *hook_params_mqtt_user(void);
const char *hook_params_mqtt_password(void);

/*
 * True for parameters whose value must never reach a log or an MQTT message
 * (mqtt.psw). Used by the offline log dump and the remote AT+PARAM read.
 */
bool hook_params_key_is_secret(const char *key);

/* Write one parameter (memory only) / persist all dirty parameters. */
int hook_params_set(const char *key, const char *value);
int hook_params_save(void);

/* Read any registered parameter by key (current in-memory value). */
int hook_params_get(const char *key, char *buf, size_t len);

/* Restore one parameter to its default (memory only, needs a save). */
int hook_params_reset(const char *key);

/*
 * Enumerate the parameters this application knows about (its own table plus
 * the identity labels), for the remote AT+PARAM listing.
 */
size_t hook_params_count(void);
const char *hook_params_key_at(size_t index);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_PARAMS_H */
