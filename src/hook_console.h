/*
 * hook_console.h - print complete text blocks on the debug console.
 */
#ifndef HOOK_CONSOLE_H
#define HOOK_CONSOLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Print one complete text block through the shell backend (uart5). Kept in a
 * single shell_fprintf() call so the line editor does not erase a partially
 * printed modem answer.
 */
int32_t hook_console_send(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_CONSOLE_H */
