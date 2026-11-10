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
| `PWD LED/供电` 上电默认低（旧板 PB3，V3 PF4） | `src/board.c`、`app/hook_heartbeat` |
| 灯光改为软件闪烁：亮 2 s / 灭 1 s（8×250 ms / 4×250 ms），由本机下发 `0x0041`/`0x0060` | `app/hook_alarm` |
| `PWD LED/供电` 在 `CMD_LED_SWITCH=1` 或 `STATUS_BUZZER!=0` 时开启（声光模块主供电由 `ALARM_POWER_EN` 单独控制） | `app/hook_alarm` |
| 声光线程周期 100 ms → 50 ms，`led_logic()` 每轮执行 | `app/hook_alarm` |
| 上电自检：播放测试音时 `PWD_LED` 亮，自检结束熄灭 | `app/hook_alarm` |
| 云端 0x06 工作模式读应答改为 2 字节（`[mode, error_code]`），写请求错误码改为 `0x04` | `app/hook_4g/hook_cloud_proto.c` |
| 软待机进入时 `PWD_LED` 熄灭 | `app/hook_rtc/hook_rtc.c` |

## 问题点汇总 1 的处理（本轮）

`docs/问题点.md` 的清单 1 已逐条处理，详见
[docs/问题点解答1.md](docs/问题点解答1.md)：

| # | 问题 | 处理 |
| --- | --- | --- |
| 1 | `system_health_service` 检测哪些健康指标 | 已在解答文档中给出事件表与内核级检查清单 |
| 2 | 4G 模块每小时重启一次 | **已删除**每小时 `usr.cn#AT+Z` 保活（`app/hook_4g`） |
| 3 | 无 GPS；RTC 改由 4G NTP、Modbus/MQTT 校准；RTC 寄存器拆分；定时开关机完善 | GPS/NMEA 代码全部删除；新增 `app/hook_4g_ntp` 和 `app/hook_rtc`（校准命令 H0x20~0x22 + 当前时间 I0x10~0x12 + 区间式定时开关机） |
| 4 | 寄存器表改用 `modbus_data_model` 子模块 | **已实现**：新增 `module/modbus_data_model` 子模块，寄存器表带名称/权限/默认值，定时开关机时间持久化到 `modbus-store` 分区 |
| 5 | 删除无用的 `activity_led_gpio` | **已删除**（dts 节点/别名、`src/board.{c,h}`、`src/hook_task.c`） |

## 与旧工程的对应关系

| 旧工程（CubeMX + FreeRTOS） | 本工程（Zephyr） |
| --- | --- |
| `crane_ai_satefy_hook.ioc` / `Core/Src/{main,gpio,usart,rtc,dma}.c` | `boards/craner/craner_hook_stm32h743vit6/`（devicetree + Kconfig） |
| `Core/Src/freertos.c` + `application/app_hook_task.c`（任务创建） | `src/main.c` + `src/hook_task.c` |
| `cmsis_os.h` / `osDelay` / `xTaskNotify` | Zephyr kernel + `src/osal_zephyr.{h,c}` |
| HAL UART + DMA 串口管理（`component/uart_manage`） | `src/hook_4g_uart.c`（Zephyr UART Async API + DMA；双缓冲 RX、20 ms 空闲分帧、串行化 TX 队列） |
| `Com/am_modbus`（RTU 主/从机） | Zephyr Modbus subsystem（`src/modbus_rtu_{slave,master}.c`） |
| `application/modbus_rtu_server_interface.c`（MB_Reg_* 寄存器） | `app/hook_modbus/`（寄存器存储 + 上位机 RTU 从机 + 两台 RTU 主机） |
| `application/app_bms_alarm.c`（声光 / BMS） | `app/hook_alarm/` + `app/hook_bms/` |
| `application/app_relay.c`（上位机心跳监督 + 声光供电） | `app/hook_heartbeat/` |
| `application/gps_app.c` 的 RTC 定时开关机 / 待机部分 | `app/hook_rtc/`（GPS/NMEA 已随本产品取消） |
| `application/modbus_rtu_server_interface.c` 的寄存器表 | `app/hook_modbus/hook_register.c`（`module/modbus_data_model` 表） |
| `application/app_4g.c` + `at_protocol_handler.c` + `uart_manage_handler.c` | `app/hook_4g/`（`hook_cloud_proto.c` + `hook_at.c` + `hook_4g.c`） |
| `application/app_sys_supervisor.c`（心跳灯动画 / 工作模式） | 心跳灯 → `system_health_service` 的 LED backend；工作模式 → `app/hook_work_mode/` |
| `application/app_4g.c` 中的错误码 / 看门狗打点（旧代码已注释） | `app/hook_health/` + `module/system_health_service`（IWDG 喂狗与任务打卡） |
| 自定义双 Bank OTA（`ota_flash_service.c` + `craner#AT+OTABOOT/OTALOCK`） | MCUboot（sysbuild, swap-using-move）+ HTTP 下载（`craner#AT+OTADL`）+ `module/ota_manager_service`（MCUmgr over shell 仍可用于本机调试） |
| `printf_redirect` / LOGD/LOGI/LOGE 宏 | Zephyr logging（`LOG_MODULE_REGISTER`，日志经 shell 输出） |
| 复位原因 / 故障记录（无） | `module/fault_dump_service` + Zephyr coredump |
| RTC 备份寄存器 magic（`RTC_BKP_MAGIC_NUMBER`） | `module/time_manager_service` 的 BBRAM trust record（CRC 保护） |

## 串口 / RS485 分配（与旧固件一致）

| 功能 | 外设 | 引脚 (TX/RX) | 波特率 | Zephyr 归属 |
| --- | --- | --- | --- | --- |
| 调试 / shell / 日志 / MCUmgr | uart5 | PB6/PB5 | 115200 | Zephyr console + shell |
| 4G / 云端透传（usr.cn） | usart1 | PA9/PA10 | 115200 | `src/hook_4g_uart.c`（DMA TX/RX） |
| 预留（原 GPS 口，本产品无 GPS 模块） | usart3 | PD8/PD9 | 115200 | 未启用（DTS `status = "disabled"`） |
| 上位机 Modbus RTU 从机（RS485） | uart8 | PE1/PE0 | 9600 | `src/modbus_rtu_slave.c` |
| BMS Modbus RTU 主机（RS485） | usart6 | PC6/PC7 | 9600 | `src/modbus_rtu_master.c` |
| 声光报警模块 RTU 主机（RS485） | uart7 | PE8/PE7 | 9600 | `src/modbus_rtu_master.c` |
| 预留（旧固件已初始化未使用） | usart2 | PD5/PD6 | 9600 | 未启用 |

旧板 RS485 端口使用自动收发方向转换芯片，因此不需要 `de-gpios`。V3 吊钩小板
使用 `PG11/PG10/PG9` 分别控制 USART6/UART7/UART8 的收发方向。V3 原理图见
[SCH_主板_2026-09-10.pdf](docs/原理图/V3/SCH_主板_2026-09-10.pdf) 和
[SCH_吊钩小板_2026-09-10.pdf](docs/原理图/V3/SCH_吊钩小板_2026-09-10.pdf)。

## 应用线程

| 线程 | 位置 | 周期 | 优先级（Zephyr，数字越小越高） |
| --- | --- | --- | --- |
| `HostHeartbeat` | `app/hook_heartbeat` | 100 ms（核心逻辑 1 s） | 1 |
| `AlarmTask` | `app/hook_alarm` | 100 ms（声光逻辑 500 ms） | 2 |
| `App4GTask` | `app/hook_4g` | 事件驱动（不再周期保活） | 2 |
| `BmsMaster` | `app/hook_bms` | 100 ms（500 ms / 10 s 轮询） | 3 |
| `RtcPwrSched` | `app/hook_rtc` | 1 s（校准/时间发布；调度检查 5 s） | 4 |
| `WorkModeTask` | `app/hook_work_mode` | 100 ms（聚合工作模式到输入寄存器 106） | 5 |
| `HookHealth` | `app/hook_health` | 1 s | 6 |
| `OtaHttp` | `app/hook_ota` | 事件驱动（HTTP 下载编排：切 HTTPD、写 slot1、切回 MQTT） | 4 |

心跳灯 `heartbeat_led`(PD10) 不再有独立线程：由 `system_health_service` 的 100 ms
周期回调驱动（`CONFIG_SYS_HEALTH_LED_INDICATOR_BACKEND=y`）——
正常 1 s 亮 / 1 s 灭；离线事件按优先级闪烁（心跳=1、声光=2、BMS=3，
每次 200 ms、间隔 200 ms、一轮后灭 2 s）；开机自检关闭。

## 编译 / 烧录 / OTA

`module/`（system_health / time_manager / ota_manager / fault_dump /
modbus_data_model / device_identity_service / device_param_server）与
`tool/zephyr-dev-workflow` 登记为 git 子模块（HKCRC 上游仓库，提交号固定在
gitlink 中，`.gitmodules` 只给出仓库地址）。克隆本工程后先初始化子模块：

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

V3 主板使用独立板级定义
`boards/craner/craner_hook_stm32h743vit6_v3/`，构建时指定：

```powershell
.\dev.ps1 build -target all -pristine -board craner_hook_stm32h743vit6_v3
```

产物位于 `build/craner_hook_stm32h743vit6_v3/`。V3 会在应用线程启动前打开
`PF3_BMQ_EN`（吊钩小板供电）和 `PA4_5V_485_EN`，RS485 方向由
`PG11/PG10/PG9` 控制。

`project_config.json` 的 `Base.ProjectName` 必须与工程目录名一致
（`ai_safety_new_hook`），sysbuild 以应用目录名生成镜像子目录；固件/产品名仍为
CMake 工程名 `craner_ai_safety_hook`。

### OTA（MCUboot + MCUmgr over shell）

本机没有以太网，OTA 走调试串口（uart5）的 MCUmgr shell 传输，使用
`mcumgr` 客户端：

> 现场走 4G/云端的固件更新统一用 **HTTP + OTA**（第 15 条）：云端在 topic 1 下发
> `craner#AT+OTADL=<url>`，模块自己切 HTTPD 下载，写 slot1，再切回 MQTT；
> `craner#AT+OTATEST` 安装、`craner#AT+OTACONFIRM` 确认。
> 旧的 **MQTT + Ymodem** 链路（`AT+OTAREQUEST` / `AT+OTABOOT`、`+OTASTATE`、
> `app/hook_ota/hook_ymodem.c`）**已整条删除**：它和 HTTP 下载抢同一个模块、
> 同一条 4G 链路和同一套 slot1 状态，实测总会互相干扰。历史设计记录见
> [docs/MQTT_Ymodem_OTA迁移方案.md](docs/MQTT_Ymodem_OTA迁移方案.md)（已废弃）。

本机调试入口：

```text
hook usr AT+Z         # 透传给 4G 模块（AT+Z 会让模块重启，仅供排查）
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
hook provision status  # 查看按 device.no 组装的 MQTT 话题/CID 与配置状态
hook provision run     # 立即执行"比对 + 差异写入"（模块会重启，10~40 s）
hook provision factory # 出厂流程：AT+CLEAR + 全量重写 + AT+S
hook craner AT+OTAINFO # 本机 craner#AT 命令（等价 craner#AT+OTAINFO）
hook ota http <url>   # 手动发起 HTTP 固件下载（等价 craner#AT+OTADL）
hook ota test <hash>  # 按 hash 安装镜像并重启（等价 craner#AT+OTATEST=<hash>）
hook ota info         # slot0/slot1 版本+hash 与 HTTP 下载状态
hook ota confirm      # 确认当前镜像（等价 craner#AT+OTACONFIRM）
hook rtc              # RTC 状态：I0x10~0x12 当前时间、H0x20~0x22 校准命令、trust
hook rtc set 2026-09-16 14:30   # 写校准寄存器（等价上位机 Modbus 写 / 云端 0x09）
hook reg i7           # 读输入寄存器 7（当前时间年），hook reg 113 读保持寄存器
hook link             # 4G 心跳质量统计（次数/平均/最大间隔/最近超时时间），clear 清零
                      # 云端等价命令：craner#AT+LINK / craner#AT+LINK=clear
id show               # 设备身份（UID、short id、MQTT client id）
param list            # 设备参数（含工厂编号 device.no）；param set/save 写入 QSPI
olog status|list|sync # SD 卡离线日志；mcumgr fs download 可远程拉取
```

`hook usr` 发送的模块应答（以及模块主动输出的 `usr.cn#...` 行）都会打印在本控制台上，
同时镜像到 MQTT；无通道前缀的纯文本帧按模块应答处理，二进制 0xA5 帧仍走云端协议解析。
4G 模块的帧格式（`num,payload` 主题分发、`usr.cn#` 命令字与 `OK/ERROR` 结束标记、
跨块重组与粘滞路由）见 [docs/4G数据帧解析.md](docs/4G数据帧解析.md)。

注意：通过 HTTP 下发的文件必须是 MCUboot 签名镜像 `zephyr.signed.bin`
（服务器需返回 `200` + `Content-Length`，不支持 302 与 chunked）。

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
hook ota info          # 查看 slot0/slot1 版本、hash 与状态
hook ota test <hash>   # 按 hash 安装镜像（MCUboot test）并重启
hook ota confirm       # 确认当前镜像
hook reset             # 冷复位
```

旧上位机 / 云端使用的 AT 命令在 4G 通道上保持兼容（`app/hook_4g/hook_at.c`）：

```text
craner#AT            -> craner#OK
craner#AT+FWTIME     -> craner#FWTIME:<编译时间>
craner#AT+OTAINFO    -> craner#+OTAINFO:slot0=...,slot1=...
craner#AT+OTADL=<url> -> craner#OK（模块切 HTTPD 下载并写入 slot1）
craner#AT+OTATEST=<hash> -> craner#OK 后重启进该镜像（MCUboot 测试）
craner#AT+OTACONFIRM -> craner#OK（工程师手动确认当前运行镜像）
craner#AT+LINK       -> craner#+LINK:<心跳统计>（=clear 清零）
craner#AT+SYSRESET   -> 冷复位
```

## 目录结构

```text
app/        产品应用（hook_modbus / hook_alarm / hook_bms / hook_heartbeat /
            hook_rtc / hook_4g / hook_ota / hook_work_mode / hook_health /
            hook_params）
boards/     自定义板级定义 craner_hook_stm32h743vit6
component/  可移植组件（offline_log_service 离线事件日志）
config/     产品配置片段（Kconfig / sysbuild）
docs/       迁移说明与寄存器映射
module/     复用服务（system_health / time_manager / ota_manager / fault_dump /
            modbus_data_model / device_identity_service / device_param_server）
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
   | app-storage | 0x081E0000 | 128 KiB | 预留（内部 Flash 不再放运行时写入的分区） |

   slot1 对齐 bank2 是为了让“应用运行中写 slot1”不触发 STM32H7 同 Bank
   读写停顿；与 `craner_ai_safety_car` 布局一致。当前固件约 164 KiB
   （app 168,184 B Flash / 124,560 B RAM，MCUboot 43,612 B）。

   运行时需要写入的数据都在**板载 W25Q64（8 MiB QSPI NOR）**上，内部 Flash 只放
   固件（与参考工程小车一致）：

   | QSPI 分区 | 偏移 | 大小 | 用途 |
   | --- | --- | --- | --- |
   | param-store | 0x000000 | 128 KiB | 预留：设备参数（Settings/ZMS） |
   | modbus-store | 0x020000 | 8 KiB | 持久化寄存器（`modbus_data_model`，两个 4 KiB bank） |
   | reserve | 0x022000 | ~8 MiB | 预留：离线日志等 |
3. **硬待机（STANDBY）**：需要 IWDG 在 Standby/Stop 下冻结的选项字节
   （旧固件 `AutoConfig_IWDG_OptionBytes()` 已写入 `IWDG_STDBY/STOP = FREEZE`）。
   Zephyr 侧不修改选项字节，量产工装需保证该配置。
4. **看门狗**：旧固件（含最新版 `.ioc` 已移除 IWDG1 初始化）实际上没有启用
   看门狗，任务打卡喂狗代码也被注释；本工程已启用
   `module/system_health_service` + `app/hook_health`：5 个业务线程每轮打卡，
   12 s 内没有完成一轮打卡则请求停止喂狗，IWDG（30 s，`CONFIG_SYS_HEALTH_WDT_TIMEOUT_MS`）
   复位系统。
   现场设备掉线（BMS / 声光 / 4G / 上位机心跳）只记录与上报错误码，不复位。
   错误码寄存器（输入寄存器 0x01）仍由检测到故障的模块单独写入（BMS / 声光 / 心跳监督各写自己的位），
   健康事件只做日志与告警，不重复改寄存器。
5. **寄存器表（2026-09-17 重排）**：按读写方向划分 —— 云端/上位机**写**的东西放
   保持寄存器（命令 0x00..、配置 0x10..、RTC 校准 0x20..），**读**的东西放输入
   寄存器（状态 0x00..、当前时间 0x10..）。状态值对外只读，云端不可能误写；Modbus
   RTU 只是调试口（上位机通过 MQTT/0xA5 访问设备状态），地址变化不影响云端协议。
   详见 `docs/寄存器映射.md`。
6. **寄存器实现**：寄存器表由 `module/modbus_data_model` 承载（名称 / 地址 /
   读写权限 / 默认值），Zephyr Modbus RTU 从机适配层直接读写该表；未在表中定义
   的地址读回 0、写被忽略。定时开关机时间（H0x10/H0x11）标记为
   `PERSISTENT`，保存到 W25Q64 的 `modbus-store` 分区（偏移 0x20000，8 KiB）：
   NOR 擦除单位 4 KiB，两个 bank 各自正好一个扇区，因此是**真正的双 bank** ——
   保存过程中掉电只会退回上一份配置，不会丢配置。
7. **配置文件名与内容**：`config/*.conf` 必须保持 ASCII 字符（PowerShell 开发
   工作流以系统 ANSI 代码页读写该文件，中文注释会导致行被破坏）。
8. **4G 通道**：usr.cn 透传前缀（`1,` / `2,` / `3,` / `4,`）、二进制 0xA5 协议
   （CRC16-CCITT，小端）与旧固件一致；MCU 侧改为中断入队 + 线程解析，避免在
   串口回调（中断上下文）里做长逻辑。
9. **RTC 校准时序**：上位机（Modbus RTU 写 H0x20/0x21/0x22）或云端（0xA5 命令
   0x09）只写“一次性校准命令”，由 `app/hook_rtc` 线程消费后再写 RTC 并重建
   `time_manager` 信任记录；未校准（trust 无效）时定时关机与硬待机不执行。
   部分写入的命令 60 s 后自动丢弃，避免半条命令被迟到补齐。
   4G MQTT 模式下，`app/hook_4g_ntp` 在开机后读取一次 `AT+CCLK`，之后每小时读取
   一次；只有模块时间晚于 2026-10-01 00:00:00 UTC+8 才写 RTC。模块自身每
   10 分钟执行一次 NTP 校时，HTTPD/OTA 期间暂停读取。
10. **组件来源**：`module/`（system_health / time_manager / ota_manager /
    fault_dump / modbus_data_model / device_identity_service / device_param_server）
    与 `tool/zephyr-dev-workflow` 已登记为 **git submodule**
    （`.gitmodules` + gitlink，提交号与参考工程 `craner_ai_safety_car` 一致），
    子模块 git 仓库位于 `.git/modules/...`，工作副本可直接离线编译。
    各依赖的上游地址、固定提交号，以及离线转换成子模块的完整步骤见
    [docs/依赖与子模块管理.md](docs/依赖与子模块管理.md)。
11. **设备标识 / 参数 / 离线日志 / 4G 质量统计**（2026-09-18 新增）：
    * 身份：`module/device_identity_service` 从 UID 派生 `short_id` / hostname /
      MQTT client id；`id show` 查看；
    * 参数：`module/device_param_server` + ZMS 存到 **QSPI `param-store` 分区**
      （`zephyr,settings-partition`），工厂编号用 `device.no`（`app/hook_params/`），
      `param set device.no ais011` + `param save` 写入；每次开机记录一次完整参数表到离线日志（`param` 分类），
      之后每次 `param save` 真正写入后再记录一次（`param <key>="<value>" (boot|saved snapshot)`，
      轮询 `device_param_server` 的 `save_count` 实现，不改子模块）；
    * 离线日志：SD 卡（SDMMC1 PC8-PC12 + PD2，FATFS `/SD:`）由
      `component/offline_log_service` 写入 `/SD:/olog/`，记录启动/复位原因、软硬待机、
      RTC 校准、BMS/声光离线恢复、4G 心跳超时、OTA 全过程；`olog status/list/sync` 现场查看，
      也可用 `mcumgr fs download` 通过 shell 串口拉取（只允许读 `/SD:/olog`）；
    * 4G 质量：主机心跳每秒一次，`hook link`（现场）/ `craner#AT+LINK`（云端）读取
      超时次数、平均/最大间隔、最近一次超时时间戳；间隔超过 20 s 记一次超时，
      且**只在心跳恢复时写一条** `4g` 分类离线日志（断链期间不刷日志，软待机不统计）；
      清零用 `hook link clear` / `craner#AT+LINK=clear`。
   详细说明与现场验证步骤见 [docs/离线日志与设备参数.md](docs/离线日志与设备参数.md)。
    资源影响见下方第 13 条（含 4G 自动配置）后的最新数字。
12. **主机心跳超时与声光报警器的联动**（2026-09-18 调整）：主机心跳静默
    `HEARTBEAT_TIMEOUT_MS`（60 s）后，控制器切断报警器供电（PE5）并关闭 PWD_LED，
    并在同一步完成：
    * 立即把声光模块上报为**离线**（置 `ERR_ALARM_OFFLINE` + 写一条 `alarm` 分类离线日志），
      原因是「主机心跳超时」，而不是再等 15 s 无应答后按“通讯超时”判离线；
    * 关闭声光模块的离线检测（`HOOK_HEALTH_EVENT_ALARM`）并**停止一切 Modbus 通讯**
      （`hook_alarm_set_suspend(true)`），不再对一个已经断电的模块反复重试
      （现场日志中的 `alarm module keepalive write failed` 不再出现）；
    * 软待机退出时若仍处于该状态，`hook_health_sleep_update(false)` 不会重新打开声光检测。

   重新收到主机心跳后按相反顺序恢复：供电 → 恢复通讯（音量/LED 缓存已作废，第一轮重新下发
    真实值）→ 重新打开离线检测（15 s 窗口从此刻起算）→ 清除 `ERR_ALARM_OFFLINE`。
    上位机把 `HEARTBEAT_ENABLE` 写 0（关闭心跳监督）时，同样立即解除该状态。
13. **4G 模块自动配置 / MQTT 话题组建**（2026-09-19 新增）：出厂不再需要工程师逐条敲
    `usr.cn#AT...`，`app/hook_4g_cfg/` 在每次 MCU 开机时执行一次：

    * 话题与 CID 由持久化参数 `device.no` 推导（`dev = "ais" + device.no`）：

      ```text
      CID    = <dev>-hook                              ais998-hook
      PUB 1  = ai_safety/<dev>/system/hook/server        2,0
      PUB 2  = ai_safety/<dev>/ota/hook/server           2,0
      PUB 3  = ai_safety/<dev>/inform/hook/device_manager 2,0
      SUB 1  = ai_safety/<dev>/system/server/hook        2
      SUB 2  = ai_safety/<dev>/ota/server/hook           2
      SUB 3  = ai_safety/<dev>/command/device_manager/hook 2
      SUB 4  = ai_safety/<dev>/heartbeat/device_manager/hook 0
      ```

    * 判据三分支：模块不应答 → 最多等 60 s（冷启动 + 注网，实测首次应答约 40 s），
      仍不应答只写日志、下次开机再试（**不**清配置）；`AT+WKMOD` 读回不含 `MQTT` →
      出厂流程 `AT+CLEAR` + 全量重写；已是 MQTT 模式 → 逐项读回比对，只写差异项。
      命令级失败（写命令/校验）重试 3 次；
    * 有写入就 `AT+S` 保存并让**模块**重启（MCU 不重启），随后等模块回来并用
      `AT+MQTTSTA` 校验 `CONNECTION`；配置期间暂停主机心跳超时动作（模块重启收不到心跳）；
    * `mqtt.svr` / `mqtt.port` / `mqtt.user` / `mqtt.psw` 与 `device.no` 都在参数表里，
      默认 `mqtt.craner.hk:1883` / `hkcrctest` / `crcHK3130`；`device.no` 的默认值是
      硬件 `short_id`，所以新板子不上工厂编号也能连上；
    * 参数集指纹存 `modem.cfg_hash`：改了 `device.no` 或任一 `mqtt.*` 就在下次开机重配一次；
      运行中改参数不会立刻重配，与手动流程一致；
    * MQTT 用户名和密码都在配置比较范围内，并通过 `AT+MQTTUSER` / `AT+MQTTPSW`
      在 `AT+S` 后读回精确比对；密码不打印到终端、不上报 MQTT，也不写入离线日志；
    * 写入 `AT+S` 后再**读回比对一次**（模块可能在写入过程中自行重启、丢掉期间的命令），
      包括密码在内的全部配置项仍有差异则重试整个流程；配置读回一致但
      `AT+MQTTSTA` 未连接时按连接/网络状态单独记录，不把配置误判为失败；
    * 全部走同步 AT 通道（`hook_4g_at_cmd()`），交换过程不打印到终端、不镜像到话题 1
      （里面含 MQTT 密码）；离线日志 `4g` 分类记录每次决策与结果；模块重启时链路
      上出现的"1~2 字节全 0"噪声帧会被丢弃，避免污染 AT 应答；
    * 配置流程只在开机跑一次（`AppCfgTask`，栈 4 KiB），保存指纹前会检查是否有
      **未 `param save` 的改动**：有就跳过指纹保存，绝不替工程师把参数写进 flash；
    * 现场命令：`hook provision status|run|factory`；云端可通过话题 1 用
      `craner#AT+PARAM` 读取/修改参数（见下一条）。
14. **云端参数访问**（2026-09-19 新增）：话题 1（远程调试口）新增 `craner#AT+PARAM` 命令，
    云端无需改动 0xA5 二进制协议即可查询与修改参数：

    ```text
    craner#AT+PARAM                     -> craner#+PARAM:<n>;device.no=998;...
    craner#AT+PARAM=device.no           -> craner#+PARAM:device.no=998
    craner#AT+PARAM=device.no,998       -> craner#OK            (只写内存, dirty)
    craner#AT+PARAM=device.no,998,save  -> craner#OK            (写入并落盘)
    craner#AT+PARAM=save                -> craner#OK            (保存所有 dirty)
    craner#AT+PARAM=reset,device.no     -> craner#OK            (恢复默认值)
    ```

    读回的 `mqtt.psw` 一律显示 `***`；key 不存在/参数非法返回 `craner#+PARAMERR:<原因>`。
    资源影响（含 13/14 条）：Flash +8.4 KB、RAM +12.1 KB（app 240,884 B / 159,896 B）。
15. **HTTP + OTA**（2026-09-22 定型）：固件操作全部走 topic 1 的 `craner#AT...` 命令，
    **不占用 0xA5 协议**（与 `AT+OTAINFO` / `AT+OTACONFIRM` 同一条通道）：

    ```text
    craner#AT+OTADL=<url> → 设备接受（HTTP 下载：URL ≤200 B、明文、200+Content-Length）
      → 挂起心跳监督 + 暂停线程打卡与 host_heartbeat 健康事件 + 屏蔽业务发送
        （HTTPD 模式下串口数据会被拼进 HTTP 请求）
      → 模块切 HTTPD（AT+HTPTP/HTPURL/HTPSV/HTPPK/HTPTIM + AT+WKMOD=HTTPD + AT+S）
      → 串口发送请求尾 "HTTP/1.1\r\nHost:<host>\r\n\r\n" 触发 GET
      → 响应体流入（按响应头里的 Content-Length 精确接收，含签名 TLV）
      → 边收边写 slot1 → 收满校验 → 状态 ready
      → 模块切回 MQTT（读回比对后只写差异项 + AT+S 保存重启，
        再确认 AT+MQTTSTA 已建链；未建链则重启模块重试一次）
      → 恢复心跳监督
      → 设备继续运行旧镜像、保持 MQTT 在线

    craner#AT+OTATEST=<hash> → 按 hash 选定镜像（必须是 slot1 那份）→
                              boot_request_upgrade(TEST) + 重启 → MCUboot 安装
    craner#AT+OTACONFIRM → 确认运行中的测试镜像（1 小时内不确认会回滚）
    craner#AT+OTAINFO   → slot0/slot1 版本 + 8 位 hash + 状态位 + HTTP 下载状态
    ```

    * **实测 246,660 B（241 KB）用时约 32 秒（约 7.7 KB/s）**，含写入 slot1 的
      flash 停顿；串口理论线速为 11.5 KB/s。比 MQTT+Ymodem 快一个数量级，
      而且下载期间完全不占用 MQTT 话题；
    * URL 要求：明文 HTTP、最长 200 字节（模块 `AT+HTPURL` 上限 256），例如
      `http://fw.craner.hk/hook/hook_1.2.0.bin`；不支持 HTTPS 和 302 重定向；
    * **按 hash 安装，不需要任何持久化状态**：`AT+OTAINFO` 每个 slot 都带
      8 位 hash，云端拿这个 hash（或直接用下载结果里的 hash）发
      `AT+OTATEST=<hash8>` 即可，判据完全来自 Flash，所以**重启后随时可用**；
      hash 必须是 slot1 里那份镜像（正在运行的 slot0 选不了，swap 后落到 slot1
      的旧镜像也只有明确指定它的 hash 才会被装回去）；
    * 状态查询：`AT+OTAINFO` 输出 `+OTAINFO:slot0=<版本>,<hash8>[,<状态>]:slot1=...`
      与 `+OTAHTTP:<0 idle|1 downloading|2 ready|3 error>,<已写>,<期望>`；
    * **下载结果自动上报**：切回 MQTT 后设备在 **topic 2**
      （`ai_safety/<dev>/ota/hook/server`，OTA 专用通道，设备→云端）
      发布一行结果，云端据此决定是否下发 `AT+OTATEST`
      （**云端需订阅该话题**；命令应答仍在 topic 1）：

      ```text
      +OTADL:OK,<响应体字节>,<镜像字节>,v<版本>,<hash 前 8 位>
      +OTADL:ERR,<原因>,<已收字节>,<期望字节>
      原因：1 URL 非法、2 模块切换 HTTPD 失败、3 无响应数据、4 接收/写入/校验失败
      ```

      **只发一条、只走 topic 2**：模块会把连续无间隔（20 ms 断帧）发出的串口数据
      合并成**一条** MQTT 消息且只认第一个通道前缀，所以"topic 1 + topic 2 各发一份"
      会让云端收到两条内容粘在一起的乱码（实测 2026-09-24 18:01）；
    * **下载与安装之间不保存任何状态**（不再使用 `ota.staged` 参数）：安装与否
      完全由"slot1 里那份镜像的 hash"决定，所以"已下载、未安装"期间即使断电/复位，
      重启后直接发 `AT+OTATEST=<hash8>` 依然有效（**不需要重新下载**）；开机若
      slot1 有镜像，控制台与离线日志会打出 `slot1 image vX.Y.Z <hash8>` 及建议的
      安装命令；
    * **下载与安装分离**：`AT+OTADL` 完成后设备继续跑旧镜像并保持 MQTT 可达，
      收到 `AT+OTATEST` 才安装，符合"设备随时可被 MQTT 访问（除正在 OTA）"的要求；
    * 现场命令同样是这套：`hook ota http <url>` / `hook ota test <hash>`；
    * **失败兜底**：无论成功失败都会把模块切回 MQTT（否则设备会失去管理通道），
      并有 10 分钟的下载总超时；下载期间 0xA5 与心跳业务静默。
