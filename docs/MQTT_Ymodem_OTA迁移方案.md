> **已废弃（2026-09-24）**：固件更新已统一改为 **HTTP + OTA**
> （`craner#AT+OTADL` / `AT+OTATEST` / `AT+OTA_CONFIRM`，见 README 第 15 条）。
> MQTT + Ymodem 这条链路（`AT+OTAREQUEST` / `AT+OTABOOT`、`+OTASTATE`、
> `app/hook_ota/hook_ymodem.c`）**已整条删除**：它与 HTTP 下载抢同一个 4G
> 模块、同一条串口链路和同一套 slot1 状态，实测两者总会互相干扰。
> 本文仅作历史设计记录保留，其中的命令与文件清单已不再对应当前代码。

# 旧 MQTT + Ymodem OTA 迁移到 MCUboot 的方案

对象：旧方案 = 云端/上位机 ⇄ MQTT（usr.cn 透传模块）⇄ 串口 ⇄ 自定义
Bootloader（`D:\CXD_WorkSpace\Items\hook\hook\craner_bootloader01`）内的
Ymodem 接收器 + 双 Bank Flash 管理。

结论：**可以保留 MQTT + Ymodem 的下发链路，只把 Flash 一侧换成 MCUboot。**
不需要移植自定义 bootloader，也不需要修改 MCUboot 源码。

> 状态：**已在本工程实现**（`app/hook_ota/`，见第 4 节）；上位机工具
> `tool-ota-mqtt-ymodem` 无需改动，只需把下发的文件换成 `zephyr.signed.bin`。

## 1. 旧方案回顾

| 环节 | 旧实现 |
| --- | --- |
| 云端通道 | MQTT（usr.cn 模块透传，MCU 只见串口字节流） |
| 串口通道约定 | `"1,"` = 普通信息 / AT 应答；`"2,"` = OTA 通道（Ymodem 数据 + 进度上报） |
| 触发 | App 收到 `craner#AT+OTABOOT` → 写 meta `ota_request=UPDATE` → 复位 |
| 下载 | Bootloader 在 `"2,"` 通道发 Ymodem `'C'`，云端按 Ymodem 1K 包发送固件 |
| 写入 | `ota_flash_write()` 以 32 字节 flashword 写入目标 slot（`ota_flash_begin/end_write`） |
| 分区 | boot 0x08000000(256K) / meta 0x08040000(256K) / slotA 0x08100000(512K) / slotB 0x08180000(512K) |
| 生效 | 文件结束 → `ota_flash_mark_pending()` → 跳转新 slot |
| 确认/回退 | `boot_count` 超过 `OTA_FLASH_CONFIRM_MAX_ATTEMPTS` 未确认 → 回退另一 slot |
| 进度 | `mqtt_ota_progress_printf()`（`"2,"` 前缀）回传 Ymodem ACK/进度 |

## 2. 迁移后的方案（MCUboot 管理固件）

```
云端 ──MQTT──> usr.cn ──串口──> hook App (Zephyr)
                                   │ 1. 收到 craner#AT+OTAREQUEST（或新命令）
                                   │ 2. 在 "2," 通道发 Ymodem 'C'
                                   │ 3. 收 Ymodem 包 → 写入 slot1（flash_area API）
                                   │ 4. 校验 → boot_request_upgrade(BOOT_UPGRADE_TEST)
                                   │ 5. 复位
                                   ▼
                              MCUboot (0x08000000)
                                   │ 校验签名 + 交换 slot0/slot1
                                   ▼
                             新固件运行
                                   │ 工程师手动确认：craner#AT+OTA_CONFIRM
                                   │ → boot_write_img_confirmed()
                                   └ 1 h 内未确认 → ota_manager_service 冷复位
                                     → MCUboot 回退到旧固件
```

### 2.1 对应关系

| 旧实现 | 新实现 |
| --- | --- |
| 自定义 bootloader（源码目录 `craner_bootloader01`） | Zephyr MCUboot（sysbuild 构建，烧一次即可） |
| meta 区（0x08040000）+ `active_slot/target_slot/boot_count` | MCUboot 镜像头/TLV + swap 状态（`swap-using-move`） |
| `ota_flash_mark_pending()` | `boot_request_upgrade(BOOT_UPGRADE_TEST)` |
| 进入 bootloader 再下载（`AT+OTABOOT`） | **不需要**：下载在应用内完成，`AT+OTABOOT` 仅保留为兼容的冷复位 |
| `ota_flash_confirm_app()`（App 30 分钟后确认） | `ota_manager_confirm_current_image()`，**由工程师手动触发**：`AT+OTA_CONFIRM` / `hook ota confirm` |
| `boot_count` 超限回退 | MCUboot test 模式 + `CONFIG_OTA_MANAGER_SERVICE_TEST_TIMEOUT_S`（默认 3600 s）自动回退 |
| `ota_flash_crc32_*` | Ymodem 包 CRC16 + 可选整镜像 CRC32/SHA（MCUboot 启动时还会验签+SHA） |
| `ota_flash_write()`（32B flashword 手写） | Zephyr `flash_area_write()`（`FIXED_PARTITION_ID(slot1_partition)`） |
| 裸 App bin | **MCUboot 签名镜像 `zephyr.signed.bin`** |

### 2.2 必须同时成立的前提（关键）

1. **云端下发的必须是签名镜像** `zephyr.signed.bin`（含 MCUboot 镜像头与 TLV）。
   直接下发裸 `zephyr.bin` 会在 MCUboot 校验阶段被拒绝。
   建议 Ymodem 文件名携带版本，例如 `hook_1.2.3.signed.bin`。
2. **写入地址必须取分区表**（`slot1_partition`），不能沿用旧的
   `0x08180000`。当前板级定义（已按此调整）：

   | 分区 | 地址 | 大小 | 说明 |
   | --- | --- | --- | --- |
   | mcuboot | 0x08000000 | 128 KiB | Bootloader（bank1） |
   | slot0 | 0x08020000 | 896 KiB | 运行镜像（bank1，可用 655,024 B） |
   | slot1 | 0x08100000 | 768 KiB | 下载镜像（**全部位于 bank2**） |
   | coredump | 0x081C0000 | 128 KiB | 故障转储 |
   | app-storage | 0x081E0000 | 128 KiB | 预留（持久化寄存器已迁到板载 QSPI Flash） |

   slot1 对齐到 bank2 边界是刻意的：App 从 bank1 运行、擦写 bank2 时
   不触发 STM32H7 的“读-写同 Bank 停顿”，OTA 期间其他线程仍能正常调度。
3. **Ymodem 接收必须在应用线程里**（不能在串口中断回调里）：
   `uart_manage` 的回调运行在 `k_timer` 上下文，不能做 Flash 擦写。
   建议新增 `app/hook_ota/`，从 `"4g"` 通道的环形缓冲取字节（`"2,"` 之后的数据）。
4. **OTA 期间要看住看门狗**：Flash 扇区擦除最大约 4 s（典型 ~1 s），
   必须在这段时间内暂停 `HOOK_HEALTH_EVENT_TASK_ALIVE` 打卡判定
   （调用 `sys_health_event_disable(HOOK_HEALTH_EVENT_TASK_ALIVE)`），
   否则 8 s 未打卡会停止喂狗并复位；下载结束后再恢复。
5. **通道约定保持不变**：`"1,"` 普通信息/AT 应答，`"2,"` OTA 数据与进度，
   这样云端 MQTT 侧基本不需要改。
6. **RAM/栈**：Ymodem 1 KiB 包 + flash 写缓冲，建议 OTA 线程栈 ≥ 4 KiB。

### 2.3 建议的命令/流程（兼容旧上位机）

| 命令（`"1,"` 通道） | 行为 |
| --- | --- |
| `craner#AT+OTAREQUEST` | 进入 OTA 待机状态，向 `"2,"` 通道发 Ymodem `'C'`（CRC 模式），开始接收 |
| `craner#AT+OTACANCEL` | 放弃本次接收，发 `CAN CAN`，清空 slot1 写入上下文 |
| `craner#AT+OTAINFO` | 返回 slot0/slot1 版本、状态（`active/pending/confirmed`） |
| `craner#AT+OTA_CONFIRM` | **工程师手动确认**当前运行镜像（`boot_write_img_confirmed()`） |
| `craner#AT+OTABOOT` | 兼容保留：冷复位（新方案不需要进 bootloader 下载） |
| `hook ota info/confirm`（本机 shell） | 与上面等价的本地调试入口 |
| `hook usr <AT 命令>`（本机 shell） | 直接向 4G 模块透传 `usr.cn#...` 命令（如 `hook usr AT+Z`），模块应答打印在控制台 |

进度上报（`"2,"` 通道）建议：

```text
"2," + <Ymodem ACK/NAK/'C'>            # 协议必需（Ymodem 握手/应答）
"2," + "PROGRESS,<received>,<total>"   # 可选，给云端 UI 显示进度
"2," + "ERROR,<code>"                  # 可选，失败原因
```

### 2.4 实施工作量（实际实现见第 4 节）

| 工作项 | 说明 | 预估 |
| --- | --- | --- |
| `app/hook_ota/hook_ymodem.c` | 移植 `ota_ymodem_protocol.c`（仅去掉 FreeRTOS/HAL 依赖） | 0.5 天 |
| `app/hook_ota/hook_ota_flash.c` | `flash_area_write()` + 镜像头/长度校验（不解析签名） | 0.5 天 |
| `app/hook_ota/hook_ota_service.c` | 状态机 + 线程 + 看门狗/健康事件门控 + 进度上报 | 0.5 天 |
| `app/hook_4g` 改动 | `"2,"` 数据在 OTA 模式下改投 OTA 环形缓冲 | 0.5 天 |
| 云端/上位机 | 下发 `zephyr.signed.bin`、Ymodem 文件名版本化、超时放大 | 与上位机团队协同 |
| 验证 | 断点续传失败重试、断电回滚、确认超时回滚 | 1~2 天 |

### 2.5 不建议的做法

- **在 MCUboot 里实现 Ymodem**：MCUboot 的串口恢复（serial recovery）有自己的
  帧格式，改造需要维护 MCUboot 分支，性价比低。
- **在设备上对 Ymodem 收到的裸 bin 做签名**：需要在 MCU 内实现 ECDSA/RSA 签名，
  不现实，且私钥不应进设备。
- **关闭镜像校验**（`BOOT_VALIDATE_SLOT0=n` 之类）：会失去 MCUboot 的核心价值。

## 3. 上位机工具 `tool-ota-mqtt-ymodem` 的兼容性

现有上位机工具（`D:\CXD_WorkSpace\Items\hook\hook\tool-ota-mqtt-ymodem`）**可以直接复用**，
它就是本方案的发送端。工具的关键行为：

| 环节 | 工具实现 | 新方案对应 |
| --- | --- | --- |
| 配置 | `src/ota_config.json`：`ota_command_topic` / `ota_publish_topic` / `ota_response_topic` | 与 usr.cn 通道映射保持一致即可，无需改代码 |
| 启动 | 向 `ota_command_topic` 周期（10 s）发布 `craner#AT+OTABOOT`，最长等 180 s | 设备收到后进入接收态并回 `'C'`（不需要重启进 bootloader） |
| 握手 | 订阅 `ota_response_topic`，等待原始字节 `'C'(0x43)` | 设备用 `"2,"` 通道回 `'C'` |
| 传输 | block0（SOH，128 B，`文件名\0大小\0`）→ `'C'/ACK` → 1 KiB 数据块（STX，0x1A 填充）→ 每块等 ACK/NAK，最多 10 次重试（单包超时 30 s） | 与 `ota_ymodem_protocol.c` 完全一致 |
| 结束 | `EOT` 等 ACK → 等 `'C'` → 空 block0 → ACK 视为成功 | 设备在收到空 block0 前完成 Flash 写入与 CRC 校验 |
| 中断 | Ctrl+Q 或 Ctrl+C：连发两个 `0x18`(CAN) | 设备收到 CAN 后放弃本次写入（保留 slot0 运行） |
| 进度 | 工具只监听响应 topic，其它字节当作日志打印 | 设备可回传进度文本，但**文本里不能出现大写字母 `C`（0x43）**，否则会被误判为握手/ACK；建议用 `PROGRESS,<received>,<total>`、`DONE`、`ERROR,<code>` |

### 3.1 使用该工具需要注意的差异

1. **选择的固件文件必须是 `zephyr.signed.bin`**（不是 `zephyr.bin`，也不是旧的裸 App bin）。
   建议文件名带版本，例如 `hook_1.2.3.signed.bin`，block0 里的文件名会一并传给设备。
2. 工具的 `mqtt_client.publish()` 使用 **QoS 2**，单包约 1 KB：需确认 usr.cn/MQTT 网关
   的 payload 上限（旧方案已按此工作）。
3. `ota_command_payload` 可保持 `craner#AT+OTABOOT`（设备端兼容实现），也可以改成
   `craner#AT+OTAREQUEST`——只要 `ota_config.json` 与设备端命令表一致。
4. 设备必须在**接收过程中忽略重复的启动命令**（工具每 10 s 重发一次），否则会重新握手
   导致传输失败；实现时参考旧 bootloader 的 `ota_manage_restart_rescue_receive()` 思路。
5. 该工具的传输**不支持断点续传**：失败后从头重传（设备侧重新擦除 slot1 尾部扇区即可）。
6. 若希望传输更快，可把工具的 `chunk_size`（默认 1024）与设备接收缓冲一起调大，
   但需同步确认 MQTT/UART 缓冲与超时参数。

### 3.2 结论

* 工具**不需要改协议**，只需把下发的文件换成 MCUboot 签名镜像；
* 设备侧新增 `app/hook_ota`（Ymodem 接收 + 写 slot1 + `boot_request_upgrade()`）后，
  链路为：`工具 → MQTT → usr.cn → usart1 "2," → hook_ota → slot1 → MCUboot 交换/校验`；
* 确认/回退由 MCUboot + `module/ota_manager_service` 完成，`craner#AT+OTA_CONFIRM` 等价于
  `hook ota confirm`。

## 4. 本工程已实现的 MCUboot OTA

### 4.1 代码位置

| 文件 | 作用 |
| --- | --- |
| `app/hook_ota/hook_ymodem.c/.h` | Ymodem 接收状态机（由 `craner_bootloader01/application/ota_ymodem_protocol.c` 1:1 移植，协议行为不变） |
| `app/hook_ota/hook_ota.c/.h` | OTA 会话状态机 + `OtaTask` 线程：握手、写 slot1、校验、`boot_request_upgrade()`、进度上报（**确认由工程师手动触发，无自动确认**） |
| `app/hook_4g/hook_4g.c` | `"2,"` 通道的数据在 OTA 会话期间改投 OTA 服务（`"3,"/"4,"` 仍走云端 0xA5 协议） |
| `app/hook_4g/hook_at.c` | `AT+OTABOOT` / `AT+OTAREQUEST` 进入接收态并回 `'C'`；`AT+OTACANCEL` 中止；`AT+OTA_CONFIRM` 手动确认；`AT+OTAINFO` 增加 `+OTASTATE` 行 |
| `src/hook_shell.c` | `hook ota start / cancel / info / confirm`，`hook status` 增加 OTA 状态 |
| `app/hook_health/hook_health.c` | `hook_health_ota_gate()`：OTA 期间暂停任务打卡判定（避免擦除期间触发看门狗复位） |
| `src/uart_manage_port.c` | `"4g"` 通道接收缓冲由 256 B 提升到 2048 B（一个 Ymodem 包 ≈1030 B 会作为一整帧到达） |
| `Kconfig` | `CONFIG_HOOK_OTA_*` 配置项 |
| `prj.conf` | `CONFIG_IMG_ERASE_PROGRESSIVELY=y`（按扇区渐进擦除，避免开头长时间阻塞握手） |

### 4.2 会话流程

```
1. 云端/工具：command topic 发 craner#AT+OTABOOT（每 10 s 重发，最长 180 s）
2. 设备：hook_at -> hook_ota_request()
        - 暂停任务打卡监督（hook_health_ota_gate(true)）
        - 在 "2," 通道发 'C'（并每秒重发，直到收到 block0 / 数据）
        - 状态 WAIT_START
3. 工具：发 block0（文件名 + 大小） -> 设备 flash_img_init(slot1)，
        回 ACK + 'C'，状态 RECEIVING
4. 工具：1 KiB 数据块，逐块等 ACK
        设备每块 flash_img_buffered_write()（按需擦除 bank2 扇区），回 ACK
        每 16 KiB 回一次 "PROGRESS,<received>,<total>"
5. 工具：EOT -> 设备回 NAK；工具重发 EOT -> 设备回 ACK + 'C'
6. 工具：空 block0 -> 设备 flush + 回 ACK
7. 设备：boot_read_bank_header(slot1) 校验 MCUboot 镜像头
        -> boot_request_upgrade(BOOT_UPGRADE_TEST)
        -> 回 "DONE"，状态 STAGED
        -> 延迟 CONFIG_HOOK_OTA_REBOOT_DELAY_MS 后 sys_reboot()
8. MCUboot：校验签名 -> 交换 slot0/slot1 -> 启动新镜像（test 模式）
9. 新镜像启动后等待**工程师手动确认**：
   `craner#AT+OTA_CONFIRM`（或本机 `hook ota confirm`）-> boot_write_img_confirmed()
   -> 若 `CONFIG_OTA_MANAGER_SERVICE_TEST_TIMEOUT_S`（默认 3600 s = 1 h）内未确认，
      module/ota_manager_service 冷复位设备 -> MCUboot 回退到旧镜像
```

### 4.3 设备在 `"2,"` 通道上的应答

| 数据 | 说明 |
| --- | --- |
| `'C'` (0x43) | CRC 模式请求（握手、以及等待数据块时每秒重发） |
| `ACK` + `'C'` (0x06 0x43) | block0 接受 / 第二个 EOT 之后 |
| `ACK` (0x06) / `NAK` (0x15) | 每个数据包的确认 / 重传请求 |
| `CAN CAN` (0x18 0x18) | 中止（错误次数超限或 `AT+OTACANCEL`） |
| `PROGRESS,<received>,<total>` | 文本进度（**不能含大写 `C`**，否则会被上位机误判为握手/ACK） |
| `DONE` | 镜像已写入 slot1 并已登记为 test 升级 |
| `ERROR,<code>` | 失败，错误码见下表 |

错误码：20 = 主机取消（收到 CAN）；21 = 协议错误/CRC/序号错误超限/超时；
22 = 会话超时（长时间没有数据）；9/10/11/12/13/14 = 内部错误
（未打开 Flash / flush 失败 / 写入长度不足 / slot1 不是有效 MCUboot 镜像 /
镜像头与文件大小不一致 / `boot_request_upgrade()` 失败）。

### 4.4 配置项

| Kconfig | 默认 | 说明 |
| --- | --- | --- |
| `CONFIG_HOOK_OTA_ENABLE` | y | 打开 OTA 服务 |
| `CONFIG_HOOK_OTA_PACKET_TIMEOUT_MS` | 30000 | 单包超时（需大于最坏扇区擦除时间 ~4 s） |
| `CONFIG_HOOK_OTA_MAX_ERRORS` | 10 | 连续错误上限，超过则 abort |
| `CONFIG_HOOK_OTA_CRC_REQUEST_INTERVAL_MS` | 1000 | 'C' 心跳间隔（消除上位机 30 s 等待） |
| `CONFIG_HOOK_OTA_SESSION_TIMEOUT_MS` | 600000 | 整会话无数据超时 |
| `CONFIG_HOOK_OTA_REBOOT_DELAY_MS` | 2000 | 上报 DONE 后到复位的时间 |
| `CONFIG_OTA_MANAGER_SERVICE_TEST_TIMEOUT_S` | 3600 | 新镜像未确认时的回退超时（1 h 后自动重启回退） |

### 4.5 台架验证步骤

1. 构建并烧录 MCUboot + 应用：

   ```powershell
   .\dev.ps1 build -target all -pristine
   .\dev.ps1 flash -target all
   ```

2. 让设备通过 4G/usr.cn 连到 MQTT Broker，确认三个 topic 与
   `tool-ota-mqtt-ymodem/src/ota_config.json` 一致。
3. 运行上位机工具并选择 **`build\craner_hook_stm32h743vit6\ai_safety_new_hook\zephyr\zephyr.signed.bin`**：

   ```powershell
   cd D:\CXD_WorkSpace\Items\hook\hook\tool-ota-mqtt-ymodem\src
   ..\.venv\Scripts\python.exe ota_cli.py
   ```

4. 设备日志应依次出现：

   ```text
   OTA state: idle -> wait_start
   file info: name=zephyr.signed.bin size=... size_valid=1
   OTA state: wait_start -> receiving
   slot1 image valid: v1.0.0 img=... B (file ... B)
   OTA staged: rebooting in 2000 ms to let MCUboot install slot1
   ```

   工具侧最后应打印 `Firmware transfer completed.`
5. 复位后确认：

   ```text
   hook ota info        # slot0/slot1 版本与状态
   hook status          # 运行状态、错误码、OTA 状态
   ```

6. 手动确认（工程师确认新固件可用）：

   ```text
   hook> hook craner AT+OTA_CONFIRM      # 或 hook ota confirm
   craner#OK
   ```

   设备日志应出现 `running image confirmed`，此后不再回退。
7. 回退验证：再走一次 OTA，但**不执行确认**。设备会在
   `CONFIG_OTA_MANAGER_SERVICE_TEST_TIMEOUT_S`（默认 3600 s = 1 h）后自动冷复位，
   MCUboot 检测到未确认的 test 镜像并回退到旧镜像。等待期间日志会周期性提示：

   ```text
   <wrn> ota_manager: Current image is not confirmed, rollback reboot armed in N s; run image confirm after validation
   ```
