/*
 * hook_relay.h - host heartbeat supervision relay.
 *
 * Ported from application/app_relay.c.
 */
#ifndef HOOK_RELAY_H
#define HOOK_RELAY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hook_relay_init(void);
void hook_relay_start(void);

/* True while the relay output is energised. */
bool hook_relay_is_on(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_RELAY_H */
