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

/* Factory device number, "" when it has not been written yet. */
const char *hook_params_device_no(void);

/* Device tag for logs / MQTT topics: device.no, else the hardware short id. */
const char *hook_params_device_tag(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_PARAMS_H */
