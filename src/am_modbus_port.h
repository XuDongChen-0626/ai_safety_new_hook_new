/*
 * am_modbus_port.h - Zephyr port configuration for the am_modbus library.
 *
 * The original library was written against FreeRTOS + STM32 HAL UART. This
 * port maps:
 *   - RTOS primitives -> Zephyr kernel objects (k_sem, k_timer, k_thread and
 *                        osal_notify_t for the FreeRTOS task notifications)
 *   - HAL UART        -> Zephyr UART driver (interrupt driven)
 *   - HAL GPIO        -> Zephyr gpio_dt_spec (optional RS485 direction pin)
 */
#ifndef THIRD_PARTY_MODBUS_LIB_CONFIG_MODBUSCONFIG_H_
#define THIRD_PARTY_MODBUS_LIB_CONFIG_MODBUSCONFIG_H_

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>
#include <stdbool.h>

#include "osal_zephyr.h"

/* Modbus TCP and USB CDC are not used on this product. */
//#define ENABLE_TCP 1
//#define ENABLE_USB_CDC 1

/* Interrupt driven RX replaces the original DMA receive-to-idle mode. */
//#define ENABLE_USART_DMA 1

/*
 * T35 is only a fallback frame gap; the port derives the real T3.5 window
 * from the configured baudrate (see modbus_uart_frame_gap_ms()).
 */
#define T35 5

#define MAX_BUFFER  256     /* communication buffer size in bytes */
#define TIMEOUT_MODBUS 1000 /* master query timeout in ms */

/* Three concurrent RTU handlers: host slave, BMS master, alarm master. */
#define MAX_M_HANDLERS 3
#define MAX_TELEGRAMS 6

#if ENABLE_TCP == 1
#define NUMBERTCPCONN   4
#define TCPAGINGCYCLES 1000
#endif

/*
 * UART abstraction used by the Modbus library. The application fills one
 * instance per physical RS485 port and passes it to ModbusInit().
 */
typedef struct
{
	const struct device *dev;           /* Zephyr UART device */
	uint32_t baudrate;                  /* e.g. 9600 */
	const struct gpio_dt_spec *en_gpio; /* RS485 direction pin (NULL = none) */
	uint8_t dataRX;                     /* scratch byte (API compatibility) */
	volatile uint8_t tx_busy;           /* TX busy flag */
} modbus_uart_t;

#endif /* THIRD_PARTY_MODBUS_LIB_CONFIG_MODBUSCONFIG_H_ */
