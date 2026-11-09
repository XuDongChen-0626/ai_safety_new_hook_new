# Hook MQTT Qt 测试上位机

该工具用于联调 Hook 的 MQTT 云端通道，位于：

```text
tool/hook_mqtt_tester/mqtt_tester.py        # PySide6 / Qt 6 图形界面
tool/hook_mqtt_tester/hook_mqtt_protocol.py
```

## 安装与运行

```powershell
python -m pip install -r tool/hook_mqtt_tester/requirements.txt
python tool/hook_mqtt_tester/mqtt_tester.py
```

依赖包括 `paho-mqtt` 和 `PySide6`。界面采用 PySide6/Qt 6 实现。

无界面的协议自测：

```powershell
python tool/hook_mqtt_tester/mqtt_tester.py --self-test
```

## 默认连接参数

```text
Broker:  mqtt.craner.hk:1883
User:    hkcrctest
Pass:    crcHK3130
Device:  999
```

界面中的设备号可以输入 `999` 或 `ais999`，工具统一转换为
`ai_safety/ais999/...`。

## Topic

```text
命令发布：ai_safety/<dev>/command/device_manager/hook
心跳发布：ai_safety/<dev>/heartbeat/device_manager/hook
AT 命令：ai_safety/<dev>/system/server/hook

业务应答订阅：ai_safety/<dev>/inform/hook/device_manager
AT 应答订阅：ai_safety/<dev>/system/hook/server
OTA 结果订阅：ai_safety/<dev>/ota/hook/server
```

云到设备的 0xA5 业务帧不带 `1,`、`3,` 等串口通道前缀。设备上行应答
可能带 `3,` 前缀，工具会自动兼容。

## 周期

```text
心跳     1.0 s，CMD 0x05，1 字节循环计数
BMS      10 s，CMD 0x02，读取 12 字节 BMS 数据
工作状态  5 s，CMD 0x06，读取 2 字节模式和错误码
```

自动发布之间至少间隔 250 ms。BMS 和状态查询故意使用不同节拍，避免与
1 Hz 心跳同时发布。

## 应答率统计

BMS 和工作状态请求会记录 `SEQ + CMD_ID + 发送时间`。只有收到相同 SEQ，
且 CMD_ID 为原请求 ID 或对应错误 ID（最高位置 1）的应答才计为成功。

界面显示：

```text
总应答率
BMS 成功应答数 / 请求数
系统状态成功应答数 / 请求数
超时数
SEQ 错配数
无对应请求的应答数
当前待应答数
最近一次请求到应答的 RTT
```

请求超过 4 秒仍未匹配即计入超时。SEQ 不一致的应答不会更新 BMS/状态显示。

## OTA

操作全部通过系统 AT topic 完成：

```text
craner#AT+OTADL=<url>
craner#AT+OTAINFO
craner#AT+OTATEST=<hash8>
craner#AT+OTACONFIRM
```

下载结果从 OTA 结果 topic 的 `+OTADL:OK,...,<hash8>` 中自动提取 hash；
也可以点击“查询 OTA 信息”从 slot1 信息中提取。
