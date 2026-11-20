# 产品18 RFID接入与调试过程

## 1. 目的

在 ais_hook 3.0 应用中为产品18（V2硬件）接入RFID读卡器，完成以下最小闭环：

1. MCU通过串口接收RFID模块主动上传的数据；
2. 解析标签UID、RSSI（dB）和电池电量等级；
3. 通过Shell查看运行状态、最新标签和实时标签流；
4. 支持在运行时将串口波特率切换为9600或115200；
5. 不引入标签离线判定、8组寄存器管理或其他业务逻辑。

## 2. 最终有效配置

| 项目 | 配置 |
| --- | --- |
| 产品 | 产品18 |
| 硬件 | V2 |
| RFID串口 | UART8 |
| TX | PE1 |
| RX | PE0 |
| 默认串口参数 | 9600 8N1 |
| 数据方向 | RFID模块主动上传，MCU接收并解析 |
| 校验和 | 当前不校验，仅标记为 `unchecked` |

本次实测证明，现场RFID模块使用 **9600 8N1** 可以正常上传标签数据。115200只保留为运行时诊断和兼容选项，不是当前产品18的默认值。

## 3. UART8资源冲突处理

ais_hook 3.0公共业务代码原本将UART8用于上位机Modbus RTU从站。V2硬件实际没有使用这条上位机Modbus链路，因此产品配置按以下方式拆分：

| 功能 | 产品18 / V2 | 产品71 / V3 |
| --- | --- | --- |
| UART8用途 | RFID | 上位机Modbus RTU从站 |
| UART8 Modbus子节点 | 禁用 | 启用 |
| 上位机Modbus从站初始化 | 禁用 | 启用 |
| 公共寄存器表 | 保留 | 保留 |
| 4G访问寄存器 | 保留 | 保留 |
| USART6 BMS Modbus主站 | 保留 | 保留 |
| UART7报警器Modbus主站 | 保留 | 保留 |

这样可以避免RFID和Modbus同时注册UART8中断回调，也不会删除公共寄存器数据模型。

## 4. 调试过程

### 4.1 初始现象

RFID模块已经上电，指示灯有动作，但使用USART2时没有收到任何数据：

```text
hook> rfid read start
{"read":"start","addr":0,"tx":"queued","confirmed":false}
hook> rfid sample show
{"tag":null}
hook> rfid status
{"ready":true,"stream":true,"err":0,"rx":0,"frames":0,"uart_err":0,"parse_err":0,"bcc_err":0,"noise":0,"rx_drop":0,"pub_drop":0,"print_drop":0}
```

其中：

- `ready=true`说明RFID驱动和UART设备初始化成功；
- `tx=queued`只说明命令已经写入UART发送队列，不代表RFID模块已经确认；
- `rx=0`说明MCU没有收到任何串口字节，因此问题不在标签帧解析器；
- `frames=0`和`tag=null`是没有接收到原始数据的后续结果。

### 4.2 串口选择调整

现场硬件连接确认RFID实际接在UART8的PE1/PE0，而不是USART2的PD5/PD6。因此将产品18的 `rfid-uart` 板级别名切换到UART8，并关闭产品18不使用的UART8 Modbus RTU从站。

### 4.3 波特率验证

驱动提供以下运行时切换命令：

```text
rfid uart baud 9600
rfid uart baud 115200
```

设置成功后返回单行JSON：

```json
{"uart":"baud","value":9600}
```

运行时设置不写入Flash，重启后恢复DTS中的默认波特率。现场最终在9600 8N1下成功收到标签，因此产品18默认保持9600。

## 5. 成功验证

### 5.1 驱动状态

```text
hook> rfid status
{"ready":true,"uart":"serial@40007c00","baud":9600,"stream":false,"err":0,"rx":511,"frames":30,"uart_err":0,"parse_err":0,"bcc_err":0,"noise":1,"rx_drop":0,"pub_drop":0,"print_drop":0}
```

状态表明：

| 字段 | 实测值 | 结论 |
| --- | ---: | --- |
| `ready` | `true` | UART和RFID接收线程正常 |
| `uart` | `serial@40007c00` | 当前使用UART8设备实例 |
| `baud` | `9600` | 当前串口速率正确 |
| `rx` | `511` | 已收到511字节原始串口数据 |
| `frames` | `30` | 已成功解析30帧标签数据 |
| `uart_err` | `0` | 没有UART硬件或帧错误 |
| `parse_err` | `0` | 没有协议字段解析错误 |
| `noise` | `1` | 丢弃了1个非帧起始字节，不影响正常读卡 |
| `rx_drop` | `0` | 接收队列没有丢字节 |
| `pub_drop` | `0` | zbus发布没有丢消息 |
| `print_drop` | `0` | Shell实时打印没有丢消息 |

### 5.2 实时标签流

开启实时输出：

```text
hook> rfid stream on
{"stream":true}
{"uid":"11025539","rssi":71,"bat":33}
{"uid":"11025542","rssi":56,"bat":33}
{"uid":"11025543","rssi":65,"bat":33}
{"uid":"11025538","rssi":54,"bat":33}
{"uid":"11025540","rssi":66,"bat":33}
{"uid":"11025541","rssi":64,"bat":33}
```

本次测试至少识别到6张不同标签。实时输出字段含义如下：

| 字段 | 含义 |
| --- | --- |
| `uid` | 4字节标签UID，Shell按8位十六进制文本输出 |
| `rssi` | 标签信号强度，单位dB |
| `bat` | 电池电量等级码；33（`0x21`）表示2.4～3.3V，24（`0x18`）表示低于2.4V；不是百分比或精确电压值 |

Shell保留`bat`原始等级用于底层诊断；A6对外上报不发送该原始值，而是转换为Flags bit0：0表示电量正常，1表示低电量。

### 5.3 持续接收与工作模式查询

继续运行后再次检查状态：

```text
hook> rfid status
{"ready":true,"uart":"serial@40007c00","baud":9600,"stream":false,"err":0,"rx":4744,"frames":279,"uart_err":0,"parse_err":0,"bcc_err":0,"noise":1,"rx_drop":0,"pub_drop":0,"print_drop":0}
```

累计接收4744字节并成功解析279帧后，UART错误、解析错误、接收丢弃、zbus发布丢弃和Shell打印丢弃仍全部为0。`noise`保持为1，没有随接收量增长，说明当前9600 8N1链路能够持续稳定接收标签数据。

查询设备工作模式：

```text
hook> rfid mode get
{"mode":0,"name":"auto","addr":5,"read_may_stop":true}
```

结果说明：

- RFID设备地址为5；
- 用户参数 `U08_u8DeviceWorkMode` 当前值为0；
- 设备当前配置为自动模式；
- 查询命令可能暂时停止正在进行的标签盘点，因此结果中保留 `read_may_stop=true` 提示。

### 5.4 工作模式设置协议

RFID协议提供工作模式设置能力，但当前Shell只实现了 `rfid mode get`，尚未实现模式设置命令。

工作模式位于用户参数地址 `0x08`，模式值如下：

| 参数值 | 模式 | 行为 |
| ---: | --- | --- |
| `0x00` | 自动模式 | 设备自动循环寻找并上传标签 |
| `0x01` | 命令模式 | 设备复位后等待上位机发送盘点命令 |
| `0x02` | 触发模式 | 外部输入触发后开始寻找标签 |

设置单个用户参数使用 `CMD_SET_USER_ONEADDR_PAR (0x21)`：

```text
Header  Command  Sequence  DeviceAddr  Len_Low  Len_High  ParamAddr  Value  BCC
1B      21       00        05          02       00        08         XX     YY
```

其中：

- `DeviceAddr=05`是本次实测查询得到的设备地址；
- `ParamAddr=08`表示工作模式参数；
- `Value`取 `00`、`01`或`02`；
- `BCC`按协议对数据区 `ParamAddr`和`Value`计算异或校验。

以设备地址5为例，三种设置帧分别为：

```text
# 自动模式，BCC = 08 XOR 00 = 08
1B 21 00 05 02 00 08 00 08

# 命令模式，BCC = 08 XOR 01 = 09
1B 21 00 05 02 00 08 01 09

# 触发模式，BCC = 08 XOR 02 = 0A
1B 21 00 05 02 00 08 02 0A
```

设置成功应收到 `0x21`成功应答，数据区返回 `0x00`；失败时返回 `0x80`、`0x81`或`0x82`。写入模式参数后，还应发送 `CMD_FREED_READER (0x01)`，让设备退出当前状态并按照用户参数中配置的工作模式运行。

后续如增加Shell设置功能，建议提供以下接口，并且必须等待写参数应答成功后再发送 `0x01`：

```text
rfid mode set auto 5
rfid mode set command 5
rfid mode set trigger 5
```

设置操作会改变RFID设备自身配置和运行行为，不应仅根据UART发送成功就报告设置成功；必须解析设备应答，并再次执行 `rfid mode get 5` 回读确认。

### 5.5 未知设备地址时的广播发现

当RFID设备地址未知时，应使用广播地址 `0x00`发送查询。当前Shell命令未指定地址时默认使用广播地址，因此可以通过下面的命令间接发现设备地址：

```text
rfid mode get
```

它等价于：

```text
rfid mode get 0
```

本次设备使用广播查询后的响应为：

```json
{"mode":0,"name":"auto","addr":5,"read_may_stop":true}
```

响应帧的 `DeviceAddr` 字段表明设备真实地址为5。

协议还允许使用 `CMD_GET_USER_ONEADDR_PAR (0x22)`直接读取用户参数 `U32_u8DeviceAddr`。该参数在用户参数表中的偏移为十进制32，即 `0x20`。广播查询帧为：

```text
1B 22 00 00 01 00 20 20
```

| 字段 | 值 | 含义 |
| --- | --- | --- |
| Header | `1B` | 帧头 |
| Command | `22` | 查询单个用户参数 |
| Sequence | `00` | 请求帧 |
| DeviceAddr | `00` | 广播地址 |
| Length | `01 00` | 数据区长度为1 |
| ParamAddr | `20` | `U32_u8DeviceAddr`参数偏移 |
| BCC | `20` | 数据区异或校验 |

成功响应的数据区为 `20 XX`，其中 `XX`是设备配置地址。实现专用Shell命令时，建议接口为：

```text
rfid addr discover
```

建议的成功输出为：

```json
{"addr":5,"confirmed":true}
```

实现时应同时比较响应帧的 `DeviceAddr` 字段和数据区返回的 `U32_u8DeviceAddr`；两者一致后才能输出 `confirmed=true`。

广播发现只适用于RS485总线上仅连接一台RFID读卡器的场景。如果多台未配置地址或未知地址的设备同时响应，RS485数据会发生碰撞；当前协议没有定义逐台防冲突枚举机制。多设备部署时应逐台接入，并预先配置互不相同的设备地址。

## 6. 常用Shell命令

```text
# 查看UART、波特率、接收计数和解析计数
rfid status

# 开启或关闭实时标签打印
rfid stream on
rfid stream off

# 查看最近一次成功解析的标签
rfid sample show

# 查看最近一次标签的完整原始帧dump
rfid sample raw

# 运行时切换串口速率
rfid uart baud 9600
rfid uart baud 115200

# 主动发送开始或停止读取命令，默认使用广播地址0
rfid read start
rfid read stop

# 查询RFID模块配置的工作模式
rfid mode get
```

## 7. 当前结论与边界

产品18已经完成“RFID串口接收、协议解析、zbus发布和Shell诊断打印”的最小读取闭环。成功配置为UART8、PE1/PE0、9600 8N1。

RFID的4G周期上传已经由独立uplink消费者实现，具体格式见《RFID 4G A6上报协议》。当前仍未实现：

- 校验和验证；
- 标签在线/离线判定；
- 8组标签寄存器维护；
- 标签去重和业务状态管理；
- 标签数据向Modbus或其他业务模块转发；
- 波特率设置持久化。

这些功能应在确认读取链路长期稳定后，根据实际业务需求独立设计，不应耦合进UART传输层或协议解析层。
