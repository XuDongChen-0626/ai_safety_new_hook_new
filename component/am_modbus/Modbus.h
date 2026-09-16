/*
 * Modbus.h - Zephyr port of the am_modbus library header.
 *
 *  Created on: May 5, 2020
 *      Author: Alejandro Mera
 */

#ifndef THIRD_PARTY_MODBUS_INC_MODBUS_H_
#define THIRD_PARTY_MODBUS_INC_MODBUS_H_

#include "am_modbus_port.h"
#include <inttypes.h>
#include <stdbool.h>

typedef enum
{
    USART_HW = 1,
    USB_CDC_HW = 2,
    TCP_HW = 3,
    USART_HW_DMA = 4,
} mb_hardware_t;

typedef enum
{
    MB_SLAVE = 3,
    MB_MASTER = 4
} mb_masterslave_t;

typedef enum
{
    ADDRESS_BROADCAST = 0,
    ADDRESS_NORMAL = 1,
} mb_address_t;

/**
 * @enum MB_FC
 * @brief Modbus function codes summary.
 */
typedef enum MB_FC
{
    MB_FC_READ_COILS = 1,
    MB_FC_READ_DISCRETE_INPUT = 2,
    MB_FC_READ_REGISTERS = 3,
    MB_FC_READ_INPUT_REGISTER = 4,
    MB_FC_WRITE_COIL = 5,
    MB_FC_WRITE_REGISTER = 6,
    MB_FC_WRITE_MULTIPLE_COILS = 15,
    MB_FC_WRITE_MULTIPLE_REGISTERS = 16
} mb_functioncode_t;

typedef struct
{
    uint8_t uxBuffer[MAX_BUFFER];
    uint8_t u8start;
    uint8_t u8end;
    uint8_t u8available;
    bool overflow;
} modbusRingBuffer_t;

/**
 * @enum MESSAGE
 * @brief Indexes to telegram frame positions
 */
typedef enum MESSAGE
{
    ID = 0,
    FUNC,
    ADD_HI,
    ADD_LO,
    NB_HI,
    NB_LO,
    BYTE_CNT
} mb_message_t;

typedef enum COM_STATES
{
    COM_IDLE = 0,
    COM_WAITING = 1,
} mb_com_state_t;

typedef enum ERR_OP_LIST
{
    ERR_NOT_MASTER = 10,
    ERR_POLLING = 11,
    ERR_BUFF_OVERFLOW = 12,
    ERR_BAD_CRC = 13,
    ERR_EXCEPTION = 14,
    ERR_BAD_SIZE = 15,
    ERR_BAD_ADDRESS = 16,
    ERR_TIME_OUT = 17,
    ERR_BAD_SLAVE_ID = 18,
    ERR_BAD_TCP_ID = 19,
    OP_OK_QUERY = 20
} mb_err_op_t;

enum
{
    EXC_FUNC_CODE = 1,
    EXC_ADDR_RANGE = 2,
    EXC_REGS_QUANT = 3,
    EXC_EXECUTE = 4
};

typedef union
{
    uint8_t u8[4];
    uint16_t u16[2];
    uint32_t u32;
} bytesFields;

/**
 * @struct modbus_t
 * @brief Master query structure.
 */
typedef struct
{
    uint8_t u8id;
    mb_functioncode_t u8fct;
    uint16_t u16RegAdd;
    uint16_t u16CoilsNo;
    uint16_t *u16reg;
    osal_notify_t *notify; /*!< Caller notification object (Zephyr port of u32CurrentTask) */
#if ENABLE_TCP == 1
    uint32_t xIpAddress;
    uint16_t u16Port;
    uint8_t u8clientID;
#endif
} modbus_t;

#if ENABLE_TCP == 1
typedef struct
{
    struct netconn *conn;
    uint32_t aging;
} tcpclients_t;
#endif

/* Telegram queue: small ring buffer protected by a Zephyr spinlock. */
typedef struct
{
    modbus_t items[MAX_TELEGRAMS];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    struct k_spinlock lock;
    struct k_sem items_avail;
} modbus_queue_t;

/**
 * @struct modbusHandler_t
 * @brief Modbus handler structure.
 */
typedef struct
{
    mb_masterslave_t uModbusType;
    modbus_uart_t *port;              /* Zephyr UART port (replaces HAL UART_HandleTypeDef) */
    uint8_t u8id;
    const struct gpio_dt_spec *en_gpio; /* RS-485 direction pin (NULL = none) */
    mb_err_op_t i8lastError;
    uint8_t u8Buffer[MAX_BUFFER];
    uint8_t u8BufferSize;
    uint8_t u8lastRec;
    uint16_t *u16regs;
    uint16_t u16InCnt, u16OutCnt, u16errCnt;
    uint16_t u16timeOut;
    uint16_t u16regsize;
    uint8_t dataRX;
    int8_t i8state;

    mb_address_t u8AddressMode;

    uint16_t *u16inputregs;
    uint16_t u16inputregsize;

    /* Zephyr kernel objects */
    modbus_queue_t QueueTelegramHandle;   /* master telegram queue */
    struct k_thread myTaskModbusAThread;  /* modbus task */
    struct k_timer xTimerT35;             /* RX frame delimiter timer */
    struct k_timer xTimerTimeout;         /* master query timeout timer */
    struct k_sem ModBusSphrHandle;        /* data access semaphore */
    osal_notify_t task_notify;            /* notification for the modbus task itself */

    modbusRingBuffer_t xBufferRX;
    mb_hardware_t xTypeHW;

#if ENABLE_TCP == 1
    tcpclients_t newconns[NUMBERTCPCONN];
    struct netconn *conn;
    uint32_t xIpAddress;
    uint16_t u16TransactionID;
    uint16_t uTcpPort;
    uint8_t newconnIndex;
#endif
} modbusHandler_t;

enum
{
    RESPONSE_SIZE = 6,
    EXCEPTION_SIZE = 3,
    CHECKSUM_SIZE = 2
};

extern modbusHandler_t *mHandlers[MAX_M_HANDLERS];

void ModbusInit(modbusHandler_t *modH);
void ModbusStart(modbusHandler_t *modH);

#if ENABLE_USB_CDC == 1
void ModbusStartCDC(modbusHandler_t *modH);
#endif

void setTimeOut(uint16_t u16timeOut);
uint16_t getTimeOut();
bool getTimeOutState();
void ModbusQuery(modbusHandler_t *modH, modbus_t telegram);
uint32_t ModbusQueryV2(modbusHandler_t *modH, modbus_t telegram);
void ModbusQueryInject(modbusHandler_t *modH, modbus_t telegram);
void StartTaskModbusSlave(void *argument1, void *argument2, void *argument3);
void StartTaskModbusMaster(void *argument1, void *argument2, void *argument3);
uint16_t calcCRC(uint8_t *Buffer, uint8_t u8length);

#if ENABLE_TCP == 1
void ModbusCloseConn(struct netconn *conn);
void ModbusCloseConnNull(modbusHandler_t *modH);
#endif

void RingAdd(modbusRingBuffer_t *xRingBuffer, uint8_t u8Val);
uint8_t RingGetAllBytes(modbusRingBuffer_t *xRingBuffer, uint8_t *buffer);
uint8_t RingGetNBytes(modbusRingBuffer_t *xRingBuffer, uint8_t *buffer, uint8_t uNumber);
uint8_t RingCountBytes(modbusRingBuffer_t *xRingBuffer);
void RingClear(modbusRingBuffer_t *xRingBuffer);

extern uint8_t numberHandlers;

int8_t ModbusSafeReadRegister(modbusHandler_t *modH, uint16_t address, uint16_t *value);
int8_t ModbusSafeWriteRegister(modbusHandler_t *modH, uint16_t address, uint16_t value);

#endif /* THIRD_PARTY_MODBUS_INC_MODBUS_H_ */
