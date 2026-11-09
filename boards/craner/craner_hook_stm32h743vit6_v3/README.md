# Craner AI Safety Hook V3 STM32H743VIT6

Board definition for the V3 hook product: `AI Safety_General_Board_V2` main
board plus the V3 hook connector board. Source schematics are in
`docs/原理图/V3/`.

## Pin map

| Function | Pins | Notes |
| --- | --- | --- |
| Debug console / shell | UART5 `PB6_TX` / `PB5_RX` | 115200 8N1 |
| 4G module | USART1 `PA9_TX` / `PA10_RX` | 115200, DMA TX/RX |
| Host Modbus RTU slave | UART8 `PE1_TX` / `PE0_RX` | direction `PG9`, 9600 8N1 |
| BMS Modbus RTU master | USART6 `PC6_TX` / `PC7_RX` | direction `PG11`, 9600 8N1 |
| Alarm Modbus RTU master | UART7 `PE8_TX` / `PE7_RX` | direction `PG10`, 9600 8N1 |
| SD card | SDMMC1 `PC8-PC12`, `PD2` | FATFS `/SD:` |
| QSPI flash | `PB2/PB10/PD11/PD12/PE2/PD13` | W25Q64, 8 MiB |
| Heart LED | `PE3_LED_CT` | active high |
| PWD LED | `PF4_WQ_CT` | active high, shared with the bridge supply control |
| Alarm supply | `PF5_CCTV_CT` / `12V_CCTV` | active high |
| Hook-board supply | `PF3_BMQ_EN` / `12V_BMQ` | active high, enabled before application threads |
| Isolated RS485 supply | `PA4_5V_485_EN` | active high, enabled before application threads |
| 4G power | `PE10_5V_4G_EN` | active high |
| 4G enable | `PG2_MCU_4G_EN` | active high |
| 4G reset / reload | `PD14` / `PD15` | active-high pulse |

The main board switches all RS485 direction pins together as
`nRE + DE`: high means transmit, low means receive.

## Build

```powershell
.\dev.ps1 build -target all -pristine -board craner_hook_stm32h743vit6_v3
```

Artifacts are generated under:

```text
build/craner_hook_stm32h743vit6_v3/mcuboot/zephyr/zephyr.hex
build/craner_hook_stm32h743vit6_v3/ai_safety_new_hook/zephyr/zephyr.signed.confirmed.hex
build/craner_hook_stm32h743vit6_v3/ai_safety_new_hook/zephyr/zephyr.signed.bin
```
