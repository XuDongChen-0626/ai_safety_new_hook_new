/*
 * hook_at.h - craner#AT command surface and usr.cn passthrough.
 *
 * Ported from application/at_protocol_handler.c.
 *
 * The commands are the device side of the OTA / diagnostics channel that the
 * cloud uses (topic 1):
 *
 *   craner#AT                  -> craner#OK
 *   craner#AT+FWTIME           -> craner#FWTIME:<build date/time>
 *   craner#AT+OTAINFO          -> craner#+OTAINFO:slot0=<ver>,<hash8>[...]:slot1=...
 *                                 (+OTAHTTP:<state>,<written>,<expected>)
 *   craner#AT+OTA_CONFIRM      -> craner#OK       (confirm running image)
 *   craner#AT+SYSRESET         -> cold reboot
 *   craner#AT+PARAM            -> craner#+PARAM:<n>;key=value;...
 *   craner#AT+PARAM=<key>      -> craner#+PARAM:<key>=<value>
 *   craner#AT+PARAM=<key>,<value>[,save]
 *   craner#AT+PARAM=save       -> persist dirty parameters
 *   craner#AT+PARAM=reset,<key>
 *   craner#AT+LINK             -> craner#+LINK:<recv>,<timeouts>,<min>,<avg>,<max>,<longest>
 *   craner#AT+LINK=clear       -> craner#OK
 *   craner#AT+OTADL=<url>      -> craner#OK   (HTTP firmware download)
 *   craner#AT+OTATEST=<hash>   -> craner#OK   (install that image + reboot,
 *                                              same rule as "image test")
 *
 * "usr.cn#AT..." lines are forwarded unchanged to the usr.cn module.
 */
#ifndef HOOK_AT_H
#define HOOK_AT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	HOOK_AT_OK = 0,
	HOOK_AT_PREFIX_NOT_MATCH = 1,
	HOOK_AT_ACTION_FAILED = 2,
	HOOK_AT_UNKNOWN_CMD = -1,
} hook_at_ret_t;

typedef int32_t (*hook_at_reply_fn_t)(const uint8_t *buf, uint16_t len);

/* Handle one "craner#AT..." line; replies (if any) go through reply_fn. */
int32_t hook_at_craner_handler(const uint8_t *buf, uint16_t len,
			       hook_at_reply_fn_t reply_fn);

/* Handle one "usr.cn#AT..." line (passthrough to the modem). */
int32_t hook_at_usr_handler(const uint8_t *buf, uint16_t len);

/*
 * Forward one line to the usr.cn module without checking its content: the
 * caller passes the complete line (including the "usr.cn#" prefix) and a
 * trailing CRLF is appended when it is missing. Used by the shell for
 * terminal driven module commands such as "usr.cn#AT+Z".
 */
int32_t hook_at_usr_forward(const uint8_t *line, uint16_t len);

/* Format "slot0/slot1 image state" for AT+OTAINFO and the shell. */
int hook_at_format_image_info(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_AT_H */
