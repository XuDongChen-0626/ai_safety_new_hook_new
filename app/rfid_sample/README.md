# RFID 读取诊断

当前仅接入产品 10 / V2：USART2，PD5 TX / PD6 RX，RS485 自动方向，9600 / 8N1。读取采用 [REW 协议第 8 章](../../docs/hardware_v2.0/datasheet/RFID协议文档.pdf) 的 `0x39` 格式，支持手动开始/停止盘点和查询配置模式；不写设备配置、不改串口参数，V3 不接入。MCU 引脚不等于外部连接器针脚编号，接线应按实际板卡确认。

数据路径为 UART 接收 → 独立协议解析器 → 接收线程发布 `rfid_sample_diagnostic_chan` → Shell 消费者。接收随应用启动常驻，当前应用组帧超时默认 250 ms；仅持续打印默认关闭。显示命令读取公共状态 API 和 zbus 消息；设备控制命令通过公共 API 提交，经协议编码器和 UART 发出。Shell 不直接操作串口或解析器内部状态。模式回复走独立响应回调，不作为标签发布，不覆盖最新标签。

## 文件结构

```text
app/rfid_sample/
├── CMakeLists.txt
├── Kconfig
├── README.md
├── sample/
│   ├── rfid_sample.c
│   └── rfid_sample.h
├── shell/
│   └── rfid_sample_shell.c
└── driver/
    ├── rfid_sample_uart.c
    ├── rfid_sample_uart.h
    ├── rfid_sample_protocol.c
    └── rfid_sample_protocol.h
```

`driver/` 中 UART 传输与纯 C 协议解析分别实现，不含发布线程或业务。接收线程、诊断消息和状态接口位于 `sample/`，Shell 消费者位于 `shell/`。模块根目录集中维护一份 CMake/Kconfig，产品 10 通过 `CONFIG_ZBUS=y` 和 `CONFIG_RFID_SAMPLE=y` 启用；当前仍是主仓库普通目录，不是 Git 子模块。协议测试位于 `tests/rfid_protocol/`，只依赖纯 C 协议文件。

## 使用

在设备 `hook>` Shell 中执行：

```text
rfid status
rfid stream on
rfid stream off
rfid sample show
rfid mode get
rfid read start
rfid read stop
```

- 持续打印默认关闭。`stream on` 只订阅后续诊断输出，不重新打开或重置串口，也不改变 RFID 设备工作模式；重复 `on` 开始新的输出会话。
- `stream off` 等待当前正在打印的一条结束，再关闭输出、丢弃待打印记录；返回后不会开始新的持续打印。生产者继续接收、解析和发布，`sample show` 仍可查询最新记录。
- `sample show` 读取 zbus 最新记录，不向设备发起请求；尚无记录时输出 `{"tag":null}`。历史记录不代表标签当前在线。
- `status` 的 `rx=0` 表示本次启动尚无串口数据；`rx` 增长而 `frames` 不增长表示尚无新的匹配完整帧，可能仍在收帧或协议不匹配。所有计数累计至本次启动，不作在线判断。

RFID 命令及持续打印均输出单行 JSON，不改变全局 Shell 提示符、其他模块日志或全局 `help`。持续打印示例：

```json
{"uid":"11008604","rssi":83,"bat":33}
```

`stream on/off` 分别返回 `{"stream":true}` / `{"stream":false}`。`rfid`、`rfid help`、`rfid -h`、`rfid --help` 返回 `cmd` 命令数组；非法参数返回 `{"error":-22}`，读取失败返回对应负错误码。旧命令 `rfid sample on/off` 和 `rfid last` 已移除。命令尾部自行按空白分词，不支持带引号参数。根命令解析失败、输入缓冲溢出等全局 Shell 错误不在模块 JSON 范围内。

`stream`和`sample show`都只输出UID、RSSI和电量等级。UID为八位十六进制字符串；`rssi`单位为dB。`bat`按协议输出十进制等级码：33（原始`0x21`）表示2.4～3.3V，24（原始`0x18`）表示低于2.4V。它不是电量百分比；由于正常区间统一上报33，也不能作为精确电压测量值。

`rfid sample raw`以单行JSON返回最近一次标签的完整原始帧十六进制dump和字节长度；它仅用于协议诊断，原始帧中的其他字节不会进入标签业务数据或A6上传字段。

### 查询与主动盘点

以下命令均可在末尾加设备地址，例如 `rfid mode get 0x12`、`rfid read start 18`。地址接受十进制或 `0x` 十六进制，范围 0～255；默认 0。0/255 为广播地址，仅允许总线上接一个 RFID 设备时使用，不支持多设备广播查询。

| 命令 | 请求帧（默认地址 00） | 行为 |
| --- | --- | --- |
| `rfid mode get [addr]` | `1B 22 00 00 01 00 08 08` | 读用户参数 U08，等待并校验回复；不写模式 |
| `rfid read start [addr]` | `1B 39 01 00 00 00 00` | 开始持续盘点；不是只读一次，不带时长参数 |
| `rfid read stop [addr]` | `1B 3B 01 00 00 00 00` | 停止盘点；MCU 接收线程仍运行 |

模式查询依据协议 3.1.21 节；成功返回例如 `{"mode":0,"name":"auto","addr":255,"read_may_stop":true}`。0/`auto` 为定时主动模式，1/`command` 为主从模式，2/`trigger` 为外部触发模式。这是**设备配置模式**，不是当前是否正在读卡。`status` 同样不能替代设备模式查询。旧固件若不支持 `0x22`，会报告设备错误或超时，不自动尝试其他命令。

**查询可能停止设备读卡**：协议第 6 页说明，除特定命令外，成功握手会停止读卡。因此回复带 `read_may_stop:true`，查询后需继续盘点时显式执行 `rfid read start`，不自动恢复、不更改模式配置。

`read start/stop` 成功仅返回 `{"read":"start","addr":0,"tx":"queued","confirmed":false}` 或对应 `stop`。这表示字节已提交 UART FIFO，不保证最后一个停止位已发出，更不表示设备已执行。第 8 章没有定义启动/停止 ACK、无卡回复或盘点结束通知，不伪造确认。通过 `stream on` 观察新标签，或检查 `frames` 是否增长；没有标签也不能据此断言命令执行失败。停止后的历史数据仍保留在 `sample show` 中。

推荐调试顺序：`rfid mode get` → `rfid stream on` → `rfid read start` → `rfid sample show`。结束设备盘点用 `read stop`；只关闭终端输出用 `stream off`。本实现不发送或解析 `0x27` 新版标签指令。

命令 API 只供线程调用，禁止从接收线程或 zbus listener 调用。一次只允许一个发送/查询，冲突返回 `-EBUSY`；UART 提交上限 100 ms，模式回复等待上限 1000 ms，超时返回 `-ETIMEDOUT`。模式请求提交或等待响应失败后 1 秒内再查询返回 `-EAGAIN`，避免立即重试误收常见迟到响应。协议 Sequence 是群组字段，不是事务号，无法绝对区分更晚到达的同命令回复；不自动重试。发送失败也可能已经发送了部分字节，不保证设备状态未改变。

模式回复只接受正确命令、匹配的目标地址（广播接受任一地址）、U08 参数和合法模式值；控制回复 BCC 错误则丢弃并累计 `bcc_err`，等到超时仍无正确回复时报告超时。设备错误回复以 `error:-EIO` 和 `device_error` 原始值返回：128 执行失败、129 无效命令、130 BCC 错误。

## 当前限制

**协议第 4 页已明确 BCC 为全部 Data 字节异或。发送命令生成BCC，模式控制回复验证BCC；标签帧仍不验证校验字节。** `sample raw`可用于查看包含校验字节的完整原始帧，但结构匹配不能保证数据完整性；丢字节或噪声若碰巧组成合法结构和长度，仍可能误识别。

不实现八组标签表、十秒离线、寄存器映射或标签使用策略，不修改GPS。MQTT周期上报由独立RFID uplink消费者负责，不属于采集Sample。

## 缓冲与统计

Shell listener 在发布线程上下文仅检查原子开关/会话代数，将消息复制进深度为 8 的静态队列，使用 `K_NO_WAIT`；不打印、不等待 Shell 锁。独立消费者线程使用 2048 字节栈、优先级 12 打印。所有资源静态分配；开关会话通过代数检查过滤切换时晚到的旧队列项。慢速终端不阻塞 UART 生产者，但会丢弃诊断打印。

分别检查以下累计值，不能统称为同一种“丢包”：

| 字段 | 含义 |
| --- | --- |
| `ready` / `stream` / `err` | UART 接收是否就绪 / 是否打印 / 初始化或停止错误码 |
| `rx` / `frames` | 已收字节 / 结构匹配的诊断帧 |
| `uart_err` / `rx_drop` | UART 错误次数 / 丢弃的接收字节 |
| `parse_err` | `sync_losses + time_field_errors`，包含候选帧拒绝、半帧超时及时间范围错误，不等于坏帧数 |
| `bcc_err` | 模式控制回复 BCC 错误次数，不是标签校验错误次数 |
| `noise` | 解析器丢弃的字节，包括错误候选和超时残片 |
| `pub_drop` | 生产者向 zbus 发布失败 |
| `print_drop` | 开启打印时 Shell 队列已满，丢弃新诊断输出 |

状态 API 的 UART 和协议统计分开采样，并非同一原子瞬间快照。`stream on/off` 不清累计统计；这些统计不构成标签健康或在线结论。

检测到 UART 错误或接收队列丢块后，下一块数据会触发半帧清理，再重新同步。这只能覆盖可观测的缺口：当前 STM32 UART 驱动可能在 FIFO 读取中自行清除 overrun 标志，因此错误计数不能证明没有发生硬件丢字节。

## 验证状态

上一版产品 10 / V2 的应用及 MCUboot 已通过 `./dev.ps1 build -target all -pristine` 编译；本次 Shell 命令和 UART 发送/模式回复扩展仅做静态检查，未重新编译。已补充请求编码和模式响应测试源码，未执行协议测试程序或实机收发验证。后续需验证连续帧、拆包/粘包、杂字节、长度错误、超时重同步、时间字段、输出开关切换、慢终端队列溢出、命令发送超时、模式回复校验及查询对盘点的影响。
