# craner_ai_safety_hook

塔吊吊钩 AI 安全控制器（hook safety controller）的 Zephyr 独立工程。

本工程由 STM32CubeIDE + FreeRTOS 固件
`D:\CXD_WorkSpace\Items\hook\hook\ai_safety_new_hook`（最新代码）
重构而来，工程框架（目录结构、板级定义、module 服务、构建/烧录/OTA 工作流）
与 `craner_ai_safety_car` 保持一致。

> 旧 Zephyr 实验工程 `ai_safety_new_hook（fault）` 已废弃，本工程未参考其代码与结构。

## 已同步的最新固件变更

相对更早的 `Items\ai_safety_new_hook` 快照，本工程已并入下列改动：

| 变更 | 本工程位置 |
| --- | --- |
| `CMD_LED_ON (0x0041)`：LED 常亮指令 | `src/system_def.h` |
| `PWD_LED`(PB3) 上电默认**低**，且不再跟随继电器 | `src/board.c`、`app/hook_relay` |
| 灯光改为软件闪烁：亮 2 s / 灭 1 s（8×250 ms / 4×250 ms），由本机下发 `0x0041`/`0x0060` | `app/hook_alarm` |
| `PWD_LED` 镜像 `CMD_LED_SWITCH`（继电器状态由 `FLASH_LIGHT_POWER_EN` 单独表示） | `app/hook_alarm` |
| 声光线程周期 100 ms → 50 ms，`led_logic()` 每轮执行 | `app/hook_alarm` |
| 上电自检：播放测试音时 `PWD_LED` 亮，自检结束熄灭 | `app/hook_alarm` |
| 云端 0x06 工作模式读应答改为 2 字节（`[mode, error_code]`），写请求错误码改为 `0x04` | `app/hook_4g/hook_cloud_proto.c` |
| 软待机进入时 `PWD_LED` 熄灭 | `app/hook_gps/hook_rtc.c` |

## 与旧工程的对应关系

| 旧工程（CubeMX + FreeRTOS） | 本工程（Zephyr） |
| --- | --- |
| `crane_ai_satefy_hook.ioc` / `Core/Src/{main,gpio,usart,rtc,dma}.c` | `boards/craner/craner_hook_stm32h743vit6/`（devicetree + Kconfig） |
| `Core/Src/freertos.c` + `application/app_hook_task.c`（任务创建） | `src/main.c` + `src/hook_task.c` |
| `cmsis_os.h` / `osDelay` / `xTaskNotify` | Zephyr kernel + `src/osal_zephyr.{h,c}` |
| HAL UART + DMA 串口管理（`component/uart_manage`） | `component/uart_manage/`（Zephyr 中断驱动移植版） |
| `Com/am_modbus`（RTU 主/从机） | `component/am_modbus/`（Zephyr 移植版） |
| `application/modbus_rtu_server_interface.c`（MB_Reg_* 寄存器） | `app/hook_modbus/`（寄存器存储 + 上位机 RTU 从机 + 两台 RTU 主机） |
| `application/app_bms_alarm.c`（声光 / BMS） | `app/hook_alarm/` + `app/hook_bms/` |
| `application/app_relay.c`（心跳继电器） | `app/hook_relay/` |
| `application/gps_app.c`（GPS + RTC 定时开关机 + 待机） | `app/hook_gps/`（`hook_gps.c` + `hook_rtc.c`） |
| `application/app_4g.c` + `at_protocol_handler.c` + `uart_manage_handler.c` | `app/hook_4g/`（`hook_cloud_proto.c` + `hook_at.c` + `hook_4g.c`） |
| `application/app_sys_supervisor.c`（心跳灯动画 / 工作模式） | `app/hook_supervisor/` |
| `application/app_4g.c` 中的错误码 / 看门狗打点（旧代码已注释） | `app/hook_health/` + `module/system_health_service`（IWDG 喂狗与任务打卡） |
| 自定义双 Bank OTA（`ota_flash_service.c` + `craner#AT+OTABOOT/OTALOCK`） | MCUboot（sysbuild, swap-using-move）+ MCUmgr（SMP over shell）+ `module/ota_manager_service` |
| `printf_redirect` / LOGD/LOGI/LOGE 宏 | Zephyr logging（`LOG_MODULE_REGISTER`，日志经 shell 输出） |
| 复位原因 / 故障记录（无） | `module/fault_dump_service` + Zephyr coredump |
| RTC 备份寄存器 magic（`RTC_BKP_MAGIC_NUMBER`） | `module/time_manager_service` 的 BBRAM trust record（CRC 保护） |

## 串口 / RS485 分配（与旧固件一致）

| 功能 | 外设 | 引脚 (TX/RX) | 波特率 | Zephyr 归属 |
| --- | --- | --- | --- | --- |
| 调试 / shell / 日志 / MCUmgr | uart5 | PB6/PB5 | 115200 | Zephyr console + shell |
| 4G / 云端透传（usr.cn） | usart1 | PA9/PA10 | 115200 | `component/uart_manage` 通道 `"4g"` |
| GPS（NMEA 授时） | usart3 | PD8/PD9 | 115200 | `component/uart_manage` 通道 `"gps"` |
| 上位机 Modbus RTU 从机（RS485） | uart8 | PE1/PE0 | 9600 | `src/modbus_rtu_slave.c` |
| BMS Modbus RTU 主机（RS485） | usart6 | PC6/PC7 | 9600 | `src/modbus_rtu_master.c` |
| 声光报警模块 RTU 主机（RS485） | uart7 | PE8/PE7 | 9600 | `src/modbus_rtu_master.c` |
| 预留（旧固件已初始化未使用） | usart2 | PD5/PD6 | 9600 | 未启用 |

RS485 端口使用自动收发方向转换芯片，因此 DTS 中不需要 `de-gpios`。
板级原理图见 [docs/SCH_H743核心板4路485_2025-05-24.pdf](docs/SCH_H743核心板4路485_2025-05-24.pdf)。

## 应用线程

| 线程 | 位置 | 周期 | 优先级（Zephyr，数字越小越高） |
| --- | --- | --- | --- |
| `RelayHeartbeat` | `app/hook_relay` | 100 ms（核心逻辑 1 s） | 1 |
| `AlarmTask` | `app/hook_alarm` | 100 ms（声光逻辑 500 ms） | 2 |
| `App4GTask` | `app/hook_4g` | 事件驱动 + 每小时保活 | 2 |
| `BmsMaster` | `app/hook_bms` | 100 ms（500 ms / 10 s 轮询） | 3 |
| `TaskModbusSlave` / `TaskModbusMaster` | `component/am_modbus` | 事件驱动 | 3 |
| `GPSStandby` | `app/hook_gps` | 100 ms（调度检查 1 s） | 4 |
| `SysSupervisor` / `WorkModeTask` | `app/hook_supervisor` | 200 ms / 100 ms | 5 |
| `HookHealth` | `app/hook_health` | 1 s | 6 |
| `OtaTask` | `app/hook_ota` | 25 ms（事件驱动，Ymodem 接收 + slot1 写入） | 3 |

## 编译 / 烧录 / OTA

首次拉取后初始化子模块（`module/` 与 `tool/zephyr-dev-workflow` 已随工程提供，
如使用 git 版本可执行）：

```powershell
git submodule update --init --recursive
```

在工程根目录执行（需要已安装 Zephyr SDK，`project_config.json` 指向
`../../zephyr` 与 `../../zephyr-sdk-1.0.1`）：

```powershell
.\dev.ps1 build -target all -pristine   # 构建 MCUboot + 应用（sysbuild）
.\dev.ps1 build                          # 增量构建应用
.\dev.ps1 flash -target all              # 烧录 MCUboot + 签名应用
.\dev.ps1 menuconfig                     # 配置界面
```

构建产物位于 `build/craner_hook_stm32h743vit6/` 下：

```text
mcuboot/zephyr/zephyr.hex                                  # bootloader
ai_safety_new_hook/zephyr/zephyr.signed.bin                # 签名应用（OTA 用）
ai_safety_new_hook/zephyr/zephyr.signed.confirmed.hex      # 已确认镜像（烧录用）
```

`project_config.json` 的 `Base.ProjectName` 必须与工程目录名一致
（`ai_safety_new_hook`），sysbuild 以应用目录名生成镜像子目录；固件/产品名仍为
CMake 工程名 `craner_ai_safety_hook`。

### OTA（MCUboot + MCUmgr over shell）

本机没有以太网，OTA 走调试串口（uart5）的 MCUmgr shell 传输，使用
`mcumgr` 客户端：

> 如果需要沿用旧上位机/云端的 **MQTT + Ymodem** 下发方式（把固件写入 slot1，
> 由 MCUboot 负责校验、交换与回退），见
> [docs/MQTT_Ymodem_OTA迁移方案.md](docs/MQTT_Ymodem_OTA迁移方案.md)。
> 该方案已实现（`app/hook_ota/`），云端只需下发 MCUboot 签名镜像
> `zephyr.signed.bin`。

### OTA（MQTT + Ymodem → MCUboot slot1，已实现）

现场走 4G/MQTT 时使用 `app/hook_ota`：

```text
上位机 tool-ota-mqtt-ymodem
   → command topic : craner#AT+OTABOOT（设备进入接收态并回 'C'）
   → OTA topic     : Ymodem 1 KiB 数据块 → 写入 slot1（bank2，运行中不阻塞）
   → response topic: 'C'/ACK/NAK、PROGRESS,DONE,ERROR,<code>
   → 设备自动 boot_request_upgrade(BOOT_UPGRADE_TEST) 并复位
   → MCUboot 校验签名并交换 slot0/slot1
   → 由工程师手动确认：craner#AT+OTA_CONFIRM（或 hook ota confirm）
   → 若 1 小时内未确认，程序自动重启，MCUboot 回退到旧固件
```

本机调试入口：

```text
hook usr AT+Z         # 透传给 4G 模块：实际发送 "usr.cn#AT+Z\r\n"
hook usr usr.cn#AT+Z  # 已是完整命令时原样发送
hook usr wk_mod       # 快捷查询：工作模式        (AT+WKMOD)
hook usr mqtt_pub_tp  # 快捷查询：MQTT 发布话题   (AT+MQTTPUBTP)
hook usr mqtt_sub_tp  # 快捷查询：MQTT 订阅话题   (AT+MQTTSUBTP)
hook usr mqtt_sta     # 快捷查询：MQTT 连接状态   (AT+MQTTSTA)
hook usr mqtt_svr     # 快捷查询：MQTT 服务器     (AT+MQTTSVR)
hook usr mqtt_user    # 快捷查询：MQTT 用户名     (AT+MQTTUSER)
hook usr mqtt_psw     # 快捷查询：MQTT 密码       (AT+MQTTPSW)
hook usr mqtt_cid     # 快捷查询：MQTT Client ID  (AT+MQTTCID)
hook usr hex on|off    # 逐帧 hex dump 开关（排查用）
hook craner AT+OTAINFO # 本机 craner#AT 命令（等价 craner#AT+OTAINFO）
hook ota start        # 手动进入接收态（等上位机发 Ymodem）
hook ota cancel       # 中止
hook ota info         # MCUboot 镜像信息 + OTA 会话状态
hook ota confirm      # 确认当前镜像（等价 craner#AT+OTA_CONFIRM）
```

`hook usr` 发送的模块应答（以及模块主动输出的 `usr.cn#...` 行）都会打印在本控制台上，
同时镜像到 MQTT；无通道前缀的纯文本帧按模块应答处理，二进制 0xA5 帧仍走云端协议解析。
4G 模块的帧格式（`num,payload` 主题分发、`usr.cn#` 命令字与 `OK/ERROR` 结束标记、
跨块重组与粘滞路由）见 [docs/4G数据帧解析.md](docs/4G数据帧解析.md)。

注意：上位机选择的文件必须是 `zephyr.signed.bin`；进度文本不能含大写字母
`C`（会被上位机误判为 Ymodem 握手/ACK）。详细设计与验证步骤见
[docs/MQTT_Ymodem_OTA迁移方案.md](docs/MQTT_Ymodem_OTA迁移方案.md) 第 4 节。

```powershell
$img = "build\craner_hook_stm32h743vit6\ai_safety_new_hook\zephyr\zephyr.signed.bin"
mcumgr --conntype serial --connstring dev=COM7,baud=115200 image upload $img
mcumgr --conntype serial --connstring dev=COM7,baud=115200 image list
mcumgr --conntype serial --connstring dev=COM7,baud=115200 image test <hash>
mcumgr --conntype serial --connstring dev=COM7,baud=115200 reset
mcumgr --conntype serial --connstring dev=COM7,baud=115200 image confirm <hash>
```

或使用本工程提供的串口 OTA 脚本：

```powershell
.\tool\hook_ota_serial.ps1 -action list
.\tool\hook_ota_serial.ps1 -action upload -port COM7
.\tool\hook_ota_serial.ps1 -action test -hash <hash>
.\tool\hook_ota_serial.ps1 -action confirm -hash <hash>
```

也可在本机 shell 上操作：

```text
hook ota info          # 查看 slot0/slot1 版本与状态
hook ota confirm       # 确认当前镜像
hook reset             # 冷复位
```

旧上位机 / 云端使用的 AT 命令在 4G 通道上保持兼容（`app/hook_4g/hook_at.c`）：

```text
craner#AT            -> craner#OK
craner#AT+FWTIME     -> craner#FWTIME:<编译时间>
craner#AT+OTAINFO    -> craner#+OTAINFO:slot0=...,slot1=...
craner#AT+OTAREQUEST -> craner#OK（允许上位机开始 MCUmgr 上传）
craner#AT+OTA_CONFIRM -> craner#OK（工程师手动确认当前运行镜像）
craner#AT+OTACANCEL  -> craner#OK
craner#AT+OTABOOT    -> craner#OK 后冷复位
craner#AT+SYSRESET   -> 冷复位
```

## 目录结构

```text
app/        产品应用（hook_modbus / hook_alarm / hook_bms / hook_relay /
            hook_gps / hook_4g / hook_ota / hook_supervisor / hook_health）
boards/     自定义板级定义 craner_hook_stm32h743vit6
component/  可移植组件（uart_manage / am_modbus / nmea）
config/     产品配置片段（Kconfig / sysbuild）
docs/       迁移说明与寄存器映射
module/     复用服务（system_health / time_manager / ota_manager / fault_dump）
src/        平台层（main / board / hook_task / osal / Modbus 端口 / 待机 / shell）
tool/       开发工作流脚本
```

## 已知差异与注意事项

1. **主频**：与旧固件一致，SYSCLK 480 MHz（HSE 25 MHz → VCO 960 MHz →
   PLL1P 480 MHz），HCLK/AXI 240 MHz，APB1/2/3/4 120 MHz（VOS0）。
   配置值与上游 Zephyr `nucleo_h743zi`（同为 STM32H743 @480 MHz）一致；
   Zephyr 的 STM32H7 时钟驱动会自动选择 VOS0 并按新的 HCLK 重设 Flash 等待周期。
2. **Flash 布局（2 MiB，swap-using-move）**：

   | 分区 | 地址 | 大小 | 备注 |
   | --- | --- | --- | --- |
   | mcuboot | 0x08000000 | 128 KiB | bootloader（bank1） |
   | slot0 | 0x08020000 | 896 KiB | 运行镜像，应用可用 655,024 B（bank1） |
   | slot1 | 0x08100000 | 768 KiB | 下载镜像，**整块位于 bank2** |
   | coredump | 0x081C0000 | 128 KiB | 故障转储 |
   | app-storage | 0x081E0000 | 128 KiB | 预留 |

   slot1 对齐 bank2 是为了让“应用运行中写 slot1”不触发 STM32H7 同 Bank
   读写停顿；与 `craner_ai_safety_car` 布局一致。当前固件约 150 KiB。
3. **硬待机（STANDBY）**：需要 IWDG 在 Standby/Stop 下冻结的选项字节
   （旧固件 `AutoConfig_IWDG_OptionBytes()` 已写入 `IWDG_STDBY/STOP = FREEZE`）。
   Zephyr 侧不修改选项字节，量产工装需保证该配置。
4. **看门狗**：旧固件（含最新版 `.ioc` 已移除 IWDG1 初始化）实际上没有启用
   看门狗，任务打卡喂狗代码也被注释；本工程用
   `module/system_health_service` + `app/hook_health` 恢复：5 个业务线程每轮打卡，
   8 s 内没有完成一轮打卡则请求停止喂狗，IWDG（20 s）复位系统。
   现场设备掉线（BMS / 声光 / 4G / GPS / 上位机心跳）只记录与上报错误码，不复位。
   错误码寄存器（113）仍由检测到故障的模块单独写入（BMS / 声光 / 继电器各写自己的位），
   健康事件只做日志与告警，不重复改寄存器。
5. **寄存器兼容**：上位机 Modbus RTU 寄存器地址与含义完全沿用旧固件
   （0..255 保持寄存器 / 输入寄存器），见 `docs/寄存器映射.md`。
6. **掉电保存**：与旧固件一致，定时开关机时间、音量、心跳使能等寄存器在每次
   上电时恢复默认值。如需持久化可启用 `device_param_server`（Settings/ZMS）。
7. **配置文件名与内容**：`config/*.conf` 必须保持 ASCII 字符（PowerShell 开发
   工作流以系统 ANSI 代码页读写该文件，中文注释会导致行被破坏）。
8. **4G 通道**：usr.cn 透传前缀（`1,` / `2,` / `3,` / `4,`）、二进制 0xA5 协议
   （CRC16-CCITT，小端）与旧固件一致；MCU 侧改为中断入队 + 线程解析，避免在
   串口回调（中断上下文）里做长逻辑。
9. **GPS 授时更严格**：只有 RMC 定位有效（状态 `A`）时才把时间写入 RTC 并
   建立 BBRAM 信任记录；无定位时 RTC 保持“不可信”，定时关机与硬待机不会执行
   （安全优先；旧固件在无定位时也会写入 RTC）。
10. **组件来源**：`module/`（system_health / time_manager / ota_manager /
    fault_dump）与 `tool/zephyr-dev-workflow` 是 HKCRC 上游仓库的本地副本
    （vendored，便于离线构建）；若需跟随上游，可改回 git submodule 并保留
    相同的目录路径。
    本工程目录当前还不是 git 仓库，因此没有 `.gitmodules`；各依赖对应的上游
    地址、提交号，以及“转换为真正的子模块”的完整步骤（含离线做法）见
    [docs/依赖与子模块管理.md](docs/依赖与子模块管理.md)。
