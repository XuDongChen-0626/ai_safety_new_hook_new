/*
 * hook_modbus.h - hook Modbus layer bring-up.
 */
#ifndef HOOK_MODBUS_H
#define HOOK_MODBUS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Prepare the register store, start the host RTU slave (uart8) and the two
 * RTU masters (usart6 BMS, uart7 sound & light alarm module).
 */
void hook_modbus_init(void);

#ifdef __cplusplus
}
#endif

#endif /* HOOK_MODBUS_H */
