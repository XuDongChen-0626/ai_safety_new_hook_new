"""Craner Hook MQTT test workstation (PySide6/Qt 6).

Run:
    python tool/hook_mqtt_tester/mqtt_tester.py

Protocol-only self-test:
    python tool/hook_mqtt_tester/mqtt_tester.py --self-test
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime
import math
import queue
import re
import sys
import time
import uuid

from hook_mqtt_protocol import (
    CMD_BMS,
    CMD_HEARTBEAT,
    CMD_WORK_MODE,
    Frame,
    FrameStreamDecoder,
    build_frame,
    decode_bms,
    decode_work_status,
    derive_topics,
    format_error_bits,
    format_protection,
    hex_dump,
    self_test,
    strip_channel_prefix,
)

try:
    import paho.mqtt.client as mqtt
    from paho.mqtt.enums import CallbackAPIVersion
except ImportError:
    mqtt = None
    CallbackAPIVersion = None

try:
    from PySide6.QtCore import Qt, QTimer
    from PySide6.QtGui import QFont, QFontDatabase, QTextCursor
    from PySide6.QtWidgets import (
        QAbstractItemView,
        QApplication,
        QCheckBox,
        QFrame,
        QGridLayout,
        QGroupBox,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QMainWindow,
        QMessageBox,
        QPlainTextEdit,
        QProgressBar,
        QPushButton,
        QSizePolicy,
        QSplitter,
        QTabWidget,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )

    QT_IMPORT_ERROR = None
except ImportError as exc:
    QT_IMPORT_ERROR = exc


MIN_AUTO_PUBLISH_GAP_S = 0.25
RESPONSE_TIMEOUT_S = 4.0


@dataclass
class ScheduledTask:
    name: str
    period_s: float
    next_s: float
    enabled: bool = True


@dataclass
class RequestStats:
    label: str
    sent: int = 0
    matched: int = 0
    timed_out: int = 0
    last_rtt_ms: float | None = None


@dataclass
class PendingRequest:
    seq: int
    cmd_id: int
    label: str
    sent_s: float
    stats: RequestStats


if QT_IMPORT_ERROR is None:

    class HookMqttTester(QMainWindow):
        def __init__(self) -> None:
            super().__init__()
            self.setWindowTitle("Craner Hook MQTT 测试上位机")
            self.resize(1380, 900)
            self.setMinimumSize(1120, 760)

            self.client = None
            self.connected = False
            self.manual_disconnect = False
            self.events: queue.Queue[tuple] = queue.Queue()
            self.inform_decoder = FrameStreamDecoder()
            self.tx_seq = 0
            self.heartbeat_value = 0
            self.last_auto_publish = 0.0
            self.tasks: dict[str, ScheduledTask] = {}
            self.active_topics: dict[str, str] = {}
            self.connection_widgets: list[QWidget] = []
            self.topics: dict[str, str] = {}
            self.pending_requests: dict[int, PendingRequest] = {}
            self.request_stats: dict[str, RequestStats] = {
                "bms": RequestStats("BMS"),
                "status": RequestStats("状态"),
            }
            self.seq_mismatch_count = 0
            self.unmatched_response_count = 0

            self.topic_rows: dict[str, tuple[str, int]] = {
                "command": ("发布", 2),
                "heartbeat": ("发布", 0),
                "system_cmd": ("发布", 2),
                "inform": ("订阅", 2),
                "system_reply": ("订阅", 2),
                "ota_reply": ("订阅", 2),
            }

            self._build_ui()
            self._refresh_topics()

            self.timer = QTimer(self)
            self.timer.setInterval(50)
            self.timer.timeout.connect(self._tick)
            self.timer.start()

        def _build_ui(self) -> None:
            root_widget = QWidget()
            self.setCentralWidget(root_widget)
            root = QVBoxLayout(root_widget)
            root.setContentsMargins(12, 12, 12, 12)
            root.setSpacing(8)

            root.addWidget(self._build_connection_group())
            root.addWidget(self._build_topic_group())

            splitter = QSplitter(Qt.Orientation.Horizontal)
            splitter.addWidget(self._build_control_tabs())
            splitter.addWidget(self._build_log_group())
            splitter.setStretchFactor(0, 2)
            splitter.setStretchFactor(1, 3)
            splitter.setSizes([540, 820])
            root.addWidget(splitter, 1)

        def _build_connection_group(self) -> QGroupBox:
            group = QGroupBox("MQTT 连接")
            grid = QGridLayout(group)
            grid.setHorizontalSpacing(8)
            grid.setVerticalSpacing(6)

            self.server_edit = QLineEdit("mqtt.craner.hk")
            self.port_edit = QLineEdit("1883")
            self.port_edit.setMaximumWidth(80)
            self.device_edit = QLineEdit("999")
            self.device_edit.setMaximumWidth(120)
            self.user_edit = QLineEdit("hkcrctest")
            self.password_edit = QLineEdit("crcHK3130")
            self.password_edit.setEchoMode(QLineEdit.EchoMode.Password)
            self.tls_check = QCheckBox("TLS")
            self.connection_widgets.extend(
                [
                    self.server_edit,
                    self.port_edit,
                    self.device_edit,
                    self.user_edit,
                    self.password_edit,
                    self.tls_check,
                ]
            )

            labels_and_widgets = [
                ("服务器", self.server_edit),
                ("端口", self.port_edit),
                ("设备号", self.device_edit),
                ("用户名", self.user_edit),
                ("密码", self.password_edit),
            ]
            column = 0
            for text, widget in labels_and_widgets:
                grid.addWidget(QLabel(text), 0, column)
                grid.addWidget(widget, 0, column + 1)
                column += 2

            grid.addWidget(self.tls_check, 0, column)
            self.connect_button = QPushButton("连接")
            self.connect_button.setObjectName("primaryButton")
            self.connect_button.clicked.connect(self._connect)
            grid.addWidget(self.connect_button, 0, column + 1)
            self.disconnect_button = QPushButton("断开")
            self.disconnect_button.setObjectName("dangerButton")
            self.disconnect_button.setEnabled(False)
            self.disconnect_button.clicked.connect(self._disconnect)
            grid.addWidget(self.disconnect_button, 0, column + 2)

            self.connection_state_label = QLabel("未连接")
            self.connection_state_label.setObjectName("connectionState")
            self.last_rx_label = QLabel("-")
            self.last_rx_label.setTextInteractionFlags(
                Qt.TextInteractionFlag.TextSelectableByMouse
            )
            grid.addWidget(QLabel("状态："), 1, 0)
            grid.addWidget(self.connection_state_label, 1, 1, 1, 2)
            grid.addWidget(QLabel("最近接收："), 1, 3)
            grid.addWidget(self.last_rx_label, 1, 4, 1, column - 2)
            grid.setColumnStretch(1, 1)
            grid.setColumnStretch(5, 1)

            self.device_edit.textChanged.connect(self._refresh_topics)
            return group

        def _build_topic_group(self) -> QGroupBox:
            group = QGroupBox("MQTT Topic")
            layout = QVBoxLayout(group)
            self.topic_table = QTableWidget(len(self.topic_rows), 3)
            self.topic_table.setHorizontalHeaderLabels(["方向", "Topic", "QoS"])
            self.topic_table.verticalHeader().setVisible(False)
            self.topic_table.setEditTriggers(
                QAbstractItemView.EditTrigger.NoEditTriggers
            )
            self.topic_table.setSelectionBehavior(
                QAbstractItemView.SelectionBehavior.SelectRows
            )
            self.topic_table.setAlternatingRowColors(True)
            header = self.topic_table.horizontalHeader()
            header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
            header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
            header.setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
            self.topic_table.setFixedHeight(178)
            layout.addWidget(self.topic_table)
            return group

        def _build_control_tabs(self) -> QTabWidget:
            tabs = QTabWidget()
            tabs.addTab(self._build_monitor_tab(), "实时监控")
            tabs.addTab(self._build_ota_tab(), "OTA 管理")
            return tabs

        def _build_monitor_tab(self) -> QWidget:
            page = QWidget()
            layout = QVBoxLayout(page)
            layout.setContentsMargins(4, 8, 4, 4)

            schedule_row = QHBoxLayout()
            schedule_row.addWidget(QLabel("自动周期："))
            self.heartbeat_enabled = QCheckBox("心跳 1 Hz")
            self.heartbeat_enabled.setChecked(True)
            self.bms_enabled = QCheckBox("BMS 0.1 Hz")
            self.bms_enabled.setChecked(True)
            self.status_enabled = QCheckBox("状态 0.2 Hz")
            self.status_enabled.setChecked(True)
            schedule_row.addWidget(self.heartbeat_enabled)
            schedule_row.addWidget(self.bms_enabled)
            schedule_row.addWidget(self.status_enabled)
            schedule_row.addStretch(1)
            layout.addLayout(schedule_row)

            counters = QHBoxLayout()
            self.heartbeat_count_label = self._counter(counters, "心跳发送", "0")
            self.bms_count_label = self._counter(counters, "BMS 应答", "0")
            self.status_count_label = self._counter(counters, "状态应答", "0")
            self.response_rate_label = self._counter(counters, "应答率", "0.0%")
            layout.addLayout(counters)

            response_stats = QGroupBox("请求 / 应答统计")
            response_layout = QGridLayout(response_stats)
            self.response_rate_progress = QProgressBar()
            self.response_rate_progress.setRange(0, 1000)
            self.response_rate_progress.setValue(0)
            self.response_rate_progress.setFormat("0.0% (0/0)")
            self.response_detail_label = QLabel(
                "BMS 0/0 | 状态 0/0 | 超时 0 | SEQ 错配 0 | 待应答 0"
            )
            self.response_detail_label.setObjectName("caption")
            self.response_detail_label.setWordWrap(True)
            reset_stats_button = QPushButton("重置统计")
            reset_stats_button.clicked.connect(self._reset_response_stats)
            response_layout.addWidget(self.response_rate_progress, 0, 0)
            response_layout.addWidget(reset_stats_button, 0, 1)
            response_layout.addWidget(self.response_detail_label, 1, 0, 1, 2)
            layout.addWidget(response_stats)

            layout.addWidget(self._build_bms_group())
            layout.addWidget(self._build_status_group())
            layout.addStretch(1)
            return page

        def _counter(self, layout: QHBoxLayout, title: str,
                     value: str) -> QLabel:
            frame = QFrame()
            frame.setObjectName("counterCard")
            box = QVBoxLayout(frame)
            box.setContentsMargins(10, 7, 10, 7)
            box.setSpacing(2)
            title_label = QLabel(title)
            title_label.setObjectName("caption")
            value_label = QLabel(value)
            value_label.setObjectName("counterValue")
            box.addWidget(title_label)
            box.addWidget(value_label)
            layout.addWidget(frame, 1)
            return value_label

        def _build_bms_group(self) -> QGroupBox:
            group = QGroupBox("BMS 电量信息（0x02，每 10 秒查询）")
            grid = QGridLayout(group)
            grid.setColumnStretch(1, 1)
            self.battery_value = self._metric(grid, 0, "电量")
            self.voltage_value = self._metric(grid, 1, "总电压")
            self.current_value = self._metric(grid, 2, "总电流")
            self.discharge_value = self._metric(grid, 3, "剩余放电")
            self.charge_value = self._metric(grid, 4, "剩余充电")
            self.protect_value = self._metric(grid, 5, "保护状态")
            self.bms_time_value = self._metric(grid, 6, "更新时间")

            self.battery_progress = QProgressBar()
            self.battery_progress.setRange(0, 10000)
            self.battery_progress.setValue(0)
            self.battery_progress.setFormat("等待 BMS 数据")
            grid.addWidget(self.battery_progress, 7, 0, 1, 2)

            button = QPushButton("立即查询 BMS")
            button.clicked.connect(self._manual_bms)
            grid.addWidget(button, 8, 0, 1, 2)
            return group

        def _build_status_group(self) -> QGroupBox:
            group = QGroupBox("系统工作状态（0x06，每 5 秒查询）")
            grid = QGridLayout(group)
            grid.setColumnStretch(1, 1)
            self.work_mode_value = self._metric(grid, 0, "工作模式")
            self.error_code_value = self._metric(grid, 1, "错误码")
            self.error_detail_value = self._metric(grid, 2, "错误详情")
            self.status_time_value = self._metric(grid, 3, "更新时间")
            self.error_detail_value.setWordWrap(True)

            button = QPushButton("立即查询系统状态")
            button.clicked.connect(self._manual_status)
            grid.addWidget(button, 4, 0, 1, 2)
            return group

        def _metric(self, grid: QGridLayout, row: int, title: str) -> QLabel:
            title_label = QLabel(f"{title}：")
            value_label = QLabel("-")
            value_label.setObjectName("metricValue")
            value_label.setTextInteractionFlags(
                Qt.TextInteractionFlag.TextSelectableByMouse
            )
            grid.addWidget(title_label, row, 0, Qt.AlignmentFlag.AlignTop)
            grid.addWidget(value_label, row, 1)
            return value_label

        def _build_ota_tab(self) -> QWidget:
            page = QWidget()
            layout = QVBoxLayout(page)
            layout.setContentsMargins(8, 10, 8, 8)

            form = QGroupBox("HTTP + OTA")
            grid = QGridLayout(form)
            grid.setColumnStretch(1, 1)
            self.ota_url_edit = QLineEdit("http://121.15.185.19:19002/s/")
            self.ota_hash_edit = QLineEdit()
            self.ota_hash_edit.setMaxLength(8)
            self.ota_hash_edit.setPlaceholderText("8 位镜像 hash")
            grid.addWidget(QLabel("固件 URL："), 0, 0)
            grid.addWidget(self.ota_url_edit, 0, 1, 1, 3)
            grid.addWidget(QLabel("镜像 hash："), 1, 0)
            grid.addWidget(self.ota_hash_edit, 1, 1)

            self.ota_progress = QProgressBar()
            self.ota_progress.setRange(0, 0)
            self.ota_progress.setVisible(False)
            self.ota_progress.setTextVisible(False)
            grid.addWidget(self.ota_progress, 1, 2, 1, 2)

            button_row = QHBoxLayout()
            download_button = QPushButton("下载固件")
            download_button.setObjectName("primaryButton")
            download_button.clicked.connect(self._ota_download)
            info_button = QPushButton("查询 OTA 信息")
            info_button.clicked.connect(self._ota_info)
            test_button = QPushButton("测试并重启")
            test_button.clicked.connect(self._ota_test)
            confirm_button = QPushButton("确认当前镜像")
            confirm_button.clicked.connect(self._ota_confirm)
            button_row.addWidget(download_button)
            button_row.addWidget(info_button)
            button_row.addWidget(test_button)
            button_row.addWidget(confirm_button)
            grid.addLayout(button_row, 2, 0, 1, 4)

            self.ota_state_label = QLabel("等待 OTA 操作")
            self.ota_state_label.setObjectName("otaState")
            self.ota_state_label.setWordWrap(True)
            self.ota_state_label.setTextInteractionFlags(
                Qt.TextInteractionFlag.TextSelectableByMouse
            )
            grid.addWidget(QLabel("状态："), 3, 0, Qt.AlignmentFlag.AlignTop)
            grid.addWidget(self.ota_state_label, 3, 1, 1, 3)
            layout.addWidget(form)

            hint = QLabel(
                "流程：下载固件 → 等待 OTA topic 返回 hash → 测试并重启 → "
                "设备运行新镜像后发送确认。"
            )
            hint.setObjectName("hint")
            hint.setWordWrap(True)
            layout.addWidget(hint)
            layout.addStretch(1)
            return page

        def _build_log_group(self) -> QGroupBox:
            group = QGroupBox("消息日志")
            layout = QVBoxLayout(group)
            button_row = QHBoxLayout()
            button_row.addStretch(1)
            clear_button = QPushButton("清空日志")
            clear_button.clicked.connect(self._clear_log)
            button_row.addWidget(clear_button)
            layout.addLayout(button_row)

            self.log_view = QPlainTextEdit()
            self.log_view.setReadOnly(True)
            self.log_view.setMaximumBlockCount(3000)
            fixed_font = QFontDatabase.systemFont(
                QFontDatabase.SystemFont.FixedFont
            )
            fixed_font.setPointSize(9)
            self.log_view.setFont(fixed_font)
            layout.addWidget(self.log_view, 1)
            return group

        def _refresh_topics(self, *_args) -> None:
            try:
                self.topics = derive_topics(self.device_edit.text())
            except ValueError:
                self.topics = {
                    key: "设备号无效" for key in self.topic_rows
                }

            for row, key in enumerate(self.topic_rows):
                direction, qos = self.topic_rows[key]
                values = (direction, self.topics[key], str(qos))
                for column, value in enumerate(values):
                    item = QTableWidgetItem(value)
                    if column != 1:
                        item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                    self.topic_table.setItem(row, column, item)

        def _connect(self) -> None:
            if mqtt is None:
                QMessageBox.critical(
                    self, "缺少依赖",
                    "未安装 paho-mqtt。\n请运行：\n"
                    "python -m pip install -r "
                    "tool/hook_mqtt_tester/requirements.txt",
                )
                return
            if self.connected or self.client is not None:
                return

            try:
                port = int(self.port_edit.text())
                if not 1 <= port <= 65535:
                    raise ValueError
            except ValueError:
                QMessageBox.warning(self, "参数错误", "MQTT 端口必须是 1~65535")
                return

            self._refresh_topics()
            if self.topics["command"] == "设备号无效":
                QMessageBox.warning(self, "参数错误", "设备号不能为空")
                return

            self.active_topics = dict(self.topics)
            self.manual_disconnect = False
            self._set_connection_label("正在连接…", "#9a6700")
            self.connect_button.setEnabled(False)

            client_id = f"hook-mqtt-tester-{uuid.uuid4().hex[:10]}"
            if CallbackAPIVersion is not None:
                client = mqtt.Client(
                    callback_api_version=CallbackAPIVersion.VERSION2,
                    client_id=client_id,
                    protocol=mqtt.MQTTv311,
                )
            else:
                client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)

            self.client = client
            client.on_connect = self._on_connect
            client.on_disconnect = self._on_disconnect
            client.on_message = self._on_message
            client.reconnect_delay_set(min_delay=1, max_delay=30)

            if self.user_edit.text():
                client.username_pw_set(
                    self.user_edit.text(), self.password_edit.text()
                )
            if self.tls_check.isChecked():
                client.tls_set()

            try:
                client.connect_async(self.server_edit.text().strip(), port,
                                     keepalive=60)
                client.loop_start()
            except Exception as exc:
                self.client = None
                self.connect_button.setEnabled(True)
                self._set_connection_label("连接失败", "#b42318")
                QMessageBox.critical(self, "MQTT 连接失败", str(exc))

        def _disconnect(self) -> None:
            self.manual_disconnect = True
            client = self.client
            self.client = None
            self._set_disconnected("已断开")
            if client is not None:
                try:
                    client.disconnect()
                    client.loop_stop()
                except Exception:
                    pass

        def _set_connection_label(self, text: str, color: str) -> None:
            self.connection_state_label.setText(text)
            self.connection_state_label.setStyleSheet(
                f"color: {color}; font-weight: 700;"
            )

        def _set_connected(self, message: str) -> None:
            self.connected = True
            for widget in self.connection_widgets:
                widget.setEnabled(False)
            self.connect_button.setEnabled(False)
            self.disconnect_button.setEnabled(True)
            self._set_connection_label("已连接", "#087f23")
            self.inform_decoder.reset()

            now = time.monotonic()
            self.tasks = {
                "heartbeat": ScheduledTask("heartbeat", 1.0, now + 0.5),
                "bms": ScheduledTask("bms", 10.0, now + 2.0),
                "status": ScheduledTask("status", 5.0, now + 4.0),
            }
            self.last_auto_publish = 0.0
            self._reset_response_stats()
            self._log("SYS", "-", message)

        def _set_disconnected(self, message: str) -> None:
            self.connected = False
            for widget in self.connection_widgets:
                widget.setEnabled(True)
            self.connect_button.setEnabled(True)
            self.disconnect_button.setEnabled(False)
            self._set_connection_label(message, "#b42318")
            self._expire_all_pending("连接断开")
            self.tasks.clear()
            self._log("SYS", "-", message)

        def _on_connect(self, client, _userdata, _flags, reason_code,
                        properties=None) -> None:
            del properties
            code = getattr(reason_code, "value", reason_code)
            if int(code) != 0:
                self.events.put(("connect_failed", f"MQTT 返回码 {code}"))
                return

            for topic, qos in (
                (self.active_topics["inform"], 2),
                (self.active_topics["system_reply"], 2),
                (self.active_topics["ota_reply"], 2),
            ):
                result, _mid = client.subscribe(topic, qos=qos)
                if int(result) != int(mqtt.MQTT_ERR_SUCCESS):
                    self.events.put(
                        ("log", "ERR", topic, f"订阅失败 rc={result}")
                    )
            self.events.put(("connected", "MQTT 已连接，3 个话题已订阅"))

        def _on_disconnect(self, *args) -> None:
            if len(args) >= 5:
                reason_code = args[3]
            elif len(args) >= 3:
                reason_code = args[2]
            else:
                reason_code = "unknown"
            code = getattr(reason_code, "value", reason_code)
            self.events.put(("disconnected", f"MQTT 连接断开，rc={code}"))

        def _on_message(self, _client, _userdata, message) -> None:
            self.events.put(("message", message.topic, bytes(message.payload)))

        def _tick(self) -> None:
            self._poll_events()
            self._expire_pending_requests()
            self._run_scheduler()

        def _poll_events(self) -> None:
            try:
                while True:
                    event = self.events.get_nowait()
                    kind = event[0]
                    if kind == "connected":
                        self._set_connected(event[1])
                    elif kind == "connect_failed":
                        self._set_disconnected(event[1])
                        self._release_client()
                    elif kind == "disconnected":
                        if not self.manual_disconnect:
                            self._set_disconnected(event[1])
                    elif kind == "log":
                        self._log(*event[1:])
                    elif kind == "message":
                        self._handle_message(event[1], event[2])
            except queue.Empty:
                pass

        def _release_client(self) -> None:
            client = self.client
            self.client = None
            if client is not None:
                try:
                    client.loop_stop()
                except Exception:
                    pass

        def _reset_response_stats(self) -> None:
            self.pending_requests.clear()
            for stats in self.request_stats.values():
                stats.sent = 0
                stats.matched = 0
                stats.timed_out = 0
                stats.last_rtt_ms = None
            self.seq_mismatch_count = 0
            self.unmatched_response_count = 0
            self._update_response_stats()

        def _update_response_stats(self) -> None:
            sent = sum(stats.sent for stats in self.request_stats.values())
            matched = sum(stats.matched for stats in self.request_stats.values())
            timed_out = sum(stats.timed_out for stats in self.request_stats.values())
            rate = (100.0 * matched / sent) if sent else 0.0

            self.response_rate_label.setText(f"{rate:.1f}%")
            self.response_rate_progress.setValue(round(rate * 10))
            self.response_rate_progress.setFormat(f"{rate:.1f}% ({matched}/{sent})")

            bms = self.request_stats["bms"]
            status = self.request_stats["status"]
            latest_rtt = max(
                (stats.last_rtt_ms for stats in self.request_stats.values()
                 if stats.last_rtt_ms is not None),
                default=None,
            )
            rtt_text = f" | 最近 RTT {latest_rtt:.0f} ms" if latest_rtt is not None else ""
            self.response_detail_label.setText(
                f"BMS {bms.matched}/{bms.sent} | "
                f"状态 {status.matched}/{status.sent} | "
                f"超时 {timed_out} | SEQ 错配 {self.seq_mismatch_count} | "
                f"无主应答 {self.unmatched_response_count} | "
                f"待应答 {len(self.pending_requests)}{rtt_text}"
            )

        def _expire_pending_requests(self) -> None:
            now = time.monotonic()
            expired = [
                (seq, request)
                for seq, request in self.pending_requests.items()
                if now - request.sent_s >= RESPONSE_TIMEOUT_S
            ]
            if not expired:
                return

            for seq, request in expired:
                self.pending_requests.pop(seq, None)
                request.stats.timed_out += 1
                self._log(
                    "WRN", "RESP",
                    f"{request.label} 请求 seq={seq} 在 "
                    f"{RESPONSE_TIMEOUT_S:.1f} 秒内未收到匹配应答",
                )
            self._update_response_stats()

        def _expire_all_pending(self, reason: str) -> None:
            if not self.pending_requests:
                return
            for seq, request in list(self.pending_requests.items()):
                self.pending_requests.pop(seq, None)
                request.stats.timed_out += 1
                self._log(
                    "WRN", "RESP",
                    f"{request.label} 请求 seq={seq} 未应答: {reason}",
                )
            self._update_response_stats()

        def _match_pending_request(self, frame: Frame) -> PendingRequest | None:
            request = self.pending_requests.get(frame.seq)
            if request is not None and frame.cmd_id in (
                request.cmd_id,
                request.cmd_id | 0x80,
            ):
                self.pending_requests.pop(frame.seq, None)
                request.stats.matched += 1
                request.stats.last_rtt_ms = (
                    time.monotonic() - request.sent_s
                ) * 1000.0
                self._log(
                    "RX", "RESP",
                    f"{request.label} seq={frame.seq} 配对成功，"
                    f"RTT={request.stats.last_rtt_ms:.0f} ms",
                )
                self._update_response_stats()
                return request

            related = [
                pending for pending in self.pending_requests.values()
                if pending.cmd_id == frame.base_cmd_id
            ]
            if related:
                self.seq_mismatch_count += 1
                expected = ", ".join(str(item.seq) for item in related)
                self._log(
                    "ERR", "RESP",
                    f"应答 seq={frame.seq} cmd=0x{frame.cmd_id:02X} "
                    f"与待应答 SEQ [{expected}] 不一致",
                )
            else:
                self.unmatched_response_count += 1
                self._log(
                    "WRN", "RESP",
                    f"收到无待应答记录的帧 seq={frame.seq} "
                    f"cmd=0x{frame.cmd_id:02X}",
                )
            self._update_response_stats()
            return None

        def _handle_message(self, topic: str, payload: bytes) -> None:
            now = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            self.last_rx_label.setText(f"{now}  {topic}")

            if topic == self.active_topics.get("inform"):
                if 0xA5 not in payload:
                    self._log("INFO", topic, self._payload_text(payload))
                    return
                frames = self.inform_decoder.feed(payload)
                self._log("RX", topic, f"{len(payload)} B  {hex_dump(payload)}")
                for frame in frames:
                    self._process_binary_frame(frame)
                return

            if topic == self.active_topics.get("ota_reply"):
                text = self._payload_text(payload)
                self.ota_progress.setVisible(False)
                self.ota_state_label.setText(text or "(空 OTA 应答)")
                self._log("OTA", topic, text)
                self._extract_ota_hash(text)
                return

            if topic == self.active_topics.get("system_reply"):
                text = self._payload_text(payload)
                self._log("RX", topic, text)
                if text.startswith("craner#ERROR") or text == "ERROR":
                    self.ota_progress.setVisible(False)
                self._extract_ota_hash(text)
                return

            self._log("RX", topic, self._payload_text(payload))

        def _payload_text(self, payload: bytes) -> str:
            body = strip_channel_prefix(payload)
            try:
                text = body.decode("utf-8")
            except UnicodeDecodeError:
                return hex_dump(body)
            return text.replace("\r", "").strip()

        def _process_binary_frame(self, frame: Frame) -> None:
            request = self._match_pending_request(frame)
            if request is None:
                return

            if frame.is_error:
                error_names = {
                    0x01: "CRC 校验失败",
                    0x02: "长度错误",
                    0x03: "未知功能码",
                    0x04: "读写权限/数值错误",
                }
                error_value = frame.data[0] if frame.data else 0
                self._log(
                    "ERR", f"{request.label} 应答",
                    f"seq={frame.seq} 设备错误 {error_value}: "
                    f"{error_names.get(error_value, '未知错误')}",
                )
                return

            if request.cmd_id == CMD_BMS:
                try:
                    bms = decode_bms(frame.data)
                except ValueError as exc:
                    self._log("ERR", "BMS", str(exc))
                    return
                battery = (f"{bms.battery_percent:.2f}%"
                           if bms.battery_percent is not None else "未知/离线")
                discharge = ("无/充电中" if bms.remain_discharge_raw == 0xFFFF
                             else f"{bms.remain_discharge_raw} min")
                charge = ("无/放电中" if bms.remain_charge_raw == 0xFFFF
                          else f"{bms.remain_charge_raw} min")
                self.battery_value.setText(battery)
                self.voltage_value.setText(f"{bms.voltage_v:.2f} V")
                self.current_value.setText(f"{bms.current_a:.2f} A")
                self.discharge_value.setText(discharge)
                self.charge_value.setText(charge)
                self.protect_value.setText(
                    f"0x{bms.protect_raw:04X}  {format_protection(bms)}"
                )
                self.bms_time_value.setText(
                    datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                )
                battery_raw = 0 if bms.battery_raw == 0xFFFF else bms.battery_raw
                self.battery_progress.setValue(min(battery_raw, 10000))
                self.battery_progress.setFormat(battery)
                self.bms_count_label.setText(
                    str(int(self.bms_count_label.text() or "0") + 1)
                )
                self._log(
                    "RX", "0x02 BMS",
                    f"电量={battery} 电压={bms.voltage_v:.2f}V "
                    f"电流={bms.current_a:.2f}A",
                )
                return

            if request.cmd_id == CMD_WORK_MODE:
                try:
                    status = decode_work_status(frame.data)
                except ValueError as exc:
                    self._log("ERR", "WORK", str(exc))
                    return
                self.work_mode_value.setText(
                    f"{status.mode} - {status.mode_text}"
                )
                self.error_code_value.setText(f"0x{status.error_code:02X}")
                self.error_detail_value.setText(
                    format_error_bits(status.error_code)
                    if status.mode == 3 else "模式非故障，错误码不参与判定"
                )
                self.status_time_value.setText(
                    datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                )
                self.status_count_label.setText(
                    str(int(self.status_count_label.text() or "0") + 1)
                )
                self._log(
                    "RX", "0x06 状态",
                    f"{status.mode_text}, error=0x{status.error_code:02X}",
                )
                return

            self._log(
                "RX", f"{request.label} 应答",
                f"seq={frame.seq} cmd=0x{frame.cmd_id:02X} "
                f"data={hex_dump(frame.data)}",
            )

        def _extract_ota_hash(self, text: str) -> None:
            for pattern in (
                r"\+OTADL:OK,[^,]+,[^,]+,v?[^,]+,\s*([0-9a-fA-F]{8})",
                r"slot1=[^,\r\n:]+,\s*([0-9a-fA-F]{8})",
                r"\b([0-9a-fA-F]{8})\s*$",
            ):
                match = re.search(pattern, text)
                if match:
                    self.ota_hash_edit.setText(match.group(1).lower())
                    return

        def _run_scheduler(self) -> None:
            if not self.connected or not self.tasks or self.client is None:
                return
            now = time.monotonic()
            enabled = {
                "heartbeat": self.heartbeat_enabled.isChecked(),
                "bms": self.bms_enabled.isChecked(),
                "status": self.status_enabled.isChecked(),
            }
            for name, task in self.tasks.items():
                task.enabled = enabled[name]

            due = sorted(
                (task for task in self.tasks.values()
                 if task.enabled and task.next_s <= now),
                key=lambda task: task.next_s,
            )
            if due and now - self.last_auto_publish >= MIN_AUTO_PUBLISH_GAP_S:
                task = due[0]
                self._run_scheduled_task(task.name)
                self.last_auto_publish = now
                task.next_s += task.period_s
                if task.next_s <= now:
                    missed = math.ceil((now - task.next_s) / task.period_s)
                    task.next_s += missed * task.period_s

        def _run_scheduled_task(self, name: str) -> None:
            if name == "heartbeat":
                self.heartbeat_value = (self.heartbeat_value + 1) & 0xFF
                self._publish_frame(
                    self.active_topics["heartbeat"], CMD_HEARTBEAT,
                    bytes([self.heartbeat_value]), qos=0,
                    label="心跳", quiet=True,
                )
                self.heartbeat_count_label.setText(
                    str(int(self.heartbeat_count_label.text() or "0") + 1)
                )
            elif name == "bms":
                self._query_bms()
            elif name == "status":
                self._query_status()

        def _query_bms(self) -> None:
            self._publish_frame(
                self.active_topics["command"], CMD_BMS, b"", qos=2,
                label="BMS 查询",
            )

        def _query_status(self) -> None:
            self._publish_frame(
                self.active_topics["command"], CMD_WORK_MODE, b"", qos=2,
                label="工作状态查询",
            )

        def _manual_bms(self) -> None:
            if self.connected:
                self._query_bms()
                if "bms" in self.tasks:
                    self.tasks["bms"].next_s = time.monotonic() + 10.0

        def _manual_status(self) -> None:
            if self.connected:
                self._query_status()
                if "status" in self.tasks:
                    self.tasks["status"].next_s = time.monotonic() + 5.0

        def _publish_frame(self, topic: str, cmd_id: int, data: bytes, *,
                           qos: int, label: str, quiet: bool = False) -> bool:
            if not self.connected or self.client is None:
                return False
            seq = self.tx_seq
            self.tx_seq = (self.tx_seq + 1) & 0xFF
            payload = build_frame(cmd_id, data, seq)
            info = self.client.publish(topic, payload, qos=qos, retain=False)
            if int(info.rc) != 0:
                self._log("ERR", topic, f"{label}发布失败 rc={info.rc}")
                return False

            stats_key = {
                CMD_BMS: "bms",
                CMD_WORK_MODE: "status",
            }.get(cmd_id)
            if stats_key is not None:
                self._expire_pending_requests()
                previous = self.pending_requests.pop(seq, None)
                if previous is not None:
                    previous.stats.timed_out += 1
                    self._log(
                        "WRN", "RESP",
                        f"{previous.label} 请求 seq={seq} 超时后被新请求覆盖",
                    )
                stats = self.request_stats[stats_key]
                stats.sent += 1
                self.pending_requests[seq] = PendingRequest(
                    seq=seq,
                    cmd_id=cmd_id,
                    label=label,
                    sent_s=time.monotonic(),
                    stats=stats,
                )
                self._update_response_stats()

            if not quiet:
                self._log(
                    "TX", topic,
                    f"{label}: seq={seq} cmd=0x{cmd_id:02X} {hex_dump(payload)}",
                )
            return True

        def _publish_text(self, topic: str, text: str, qos: int,
                          label: str) -> bool:
            if not self.connected or self.client is None:
                QMessageBox.warning(self, "未连接", "请先连接 MQTT 服务器")
                return False
            info = self.client.publish(topic, text, qos=qos, retain=False)
            if int(info.rc) != 0:
                self._log("ERR", topic, f"{label}发布失败 rc={info.rc}")
                return False
            self._log("TX", topic, f"{label}: {text}")
            return True

        def _ota_download(self) -> None:
            url = self.ota_url_edit.text().strip()
            if not url:
                QMessageBox.warning(self, "OTA", "请输入固件 URL")
                return
            if self._publish_text(
                self.active_topics["system_cmd"],
                f"craner#AT+OTADL={url}", 2, "OTA 下载",
            ):
                self.ota_progress.setVisible(True)
                self.ota_state_label.setText(
                    "已发送下载命令，设备将切换 HTTPD，完成后自动恢复 MQTT。"
                )

        def _ota_info(self) -> None:
            self._publish_text(
                self.active_topics["system_cmd"],
                "craner#AT+OTAINFO", 2, "OTA 信息",
            )

        def _ota_test(self) -> None:
            image_hash = self.ota_hash_edit.text().strip()
            if not re.fullmatch(r"[0-9a-fA-F]{8}", image_hash):
                QMessageBox.warning(self, "OTA", "镜像 hash 必须是 8 位十六进制")
                return
            answer = QMessageBox.question(
                self, "OTA 测试",
                f"将安装 slot1 镜像 {image_hash} 并重启设备，是否继续？",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
            self.ota_state_label.setText(
                f"已请求测试镜像 {image_hash}，设备即将重启"
            )
            self._publish_text(
                self.active_topics["system_cmd"],
                f"craner#AT+OTATEST={image_hash}", 2, "OTA 测试",
            )

        def _ota_confirm(self) -> None:
            answer = QMessageBox.question(
                self, "OTA 确认",
                "确认当前运行镜像，取消 MCUboot 回滚？",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
            if self._publish_text(
                self.active_topics["system_cmd"],
                "craner#AT+OTACONFIRM", 2, "OTA 确认",
            ):
                self.ota_state_label.setText("已发送镜像确认命令")

        def _log(self, tag: str, topic: str, message: str) -> None:
            timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            self.log_view.appendPlainText(
                f"[{timestamp}] [{tag:<4}] {topic} | {message}"
            )
            cursor = self.log_view.textCursor()
            cursor.movePosition(QTextCursor.MoveOperation.End)
            self.log_view.setTextCursor(cursor)

        def _clear_log(self) -> None:
            self.log_view.clear()

        def closeEvent(self, event) -> None:
            self._disconnect()
            event.accept()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Craner Hook MQTT test workstation"
    )
    parser.add_argument(
        "--self-test", action="store_true",
        help="run protocol tests without GUI",
    )
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("Hook MQTT tester self-test: OK")
        return 0

    if QT_IMPORT_ERROR is not None:
        print(
            "Missing PySide6. Install with:\n"
            "  python -m pip install -r "
            "tool/hook_mqtt_tester/requirements.txt\n"
            f"Import error: {QT_IMPORT_ERROR}",
            file=sys.stderr,
        )
        return 2
    if mqtt is None:
        print(
            "Missing paho-mqtt. Install with:\n"
            "  python -m pip install -r "
            "tool/hook_mqtt_tester/requirements.txt",
            file=sys.stderr,
        )
        return 2

    app = QApplication(sys.argv[:1])
    app.setApplicationName("Craner Hook MQTT Tester")
    app.setStyle("Fusion")
    app.setStyleSheet(
        """
        QMainWindow, QWidget { background: #f3f5f7; color: #1f2933; }
        QGroupBox {
            background: #ffffff;
            border: 1px solid #d8dee5;
            border-radius: 7px;
            margin-top: 9px;
            padding-top: 9px;
            font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 5px;
            color: #34495e;
        }
        QLineEdit {
            background: #ffffff;
            border: 1px solid #c8d1db;
            border-radius: 5px;
            padding: 5px 7px;
        }
        QLineEdit:focus { border: 1px solid #1976d2; }
        QPushButton {
            background: #e8edf2;
            border: 1px solid #c5ced8;
            border-radius: 5px;
            padding: 6px 12px;
            font-weight: 600;
        }
        QPushButton:hover { background: #dde5ee; }
        QPushButton:disabled { color: #9aa5b1; background: #f1f3f5; }
        QPushButton#primaryButton { background: #1976d2; color: white; }
        QPushButton#primaryButton:hover { background: #1565c0; }
        QPushButton#dangerButton { background: #c62828; color: white; }
        QPushButton#dangerButton:hover { background: #ad1f1f; }
        QTabWidget::pane {
            background: #ffffff;
            border: 1px solid #d8dee5;
            border-radius: 6px;
        }
        QTabBar::tab {
            background: #e6ebf0;
            padding: 7px 18px;
            margin-right: 3px;
            border-top-left-radius: 5px;
            border-top-right-radius: 5px;
        }
        QTabBar::tab:selected { background: #ffffff; color: #1565c0; }
        QTableWidget {
            background: #ffffff;
            alternate-background-color: #f7f9fb;
            border: 1px solid #d8dee5;
            gridline-color: #edf0f3;
        }
        QHeaderView::section {
            background: #e9eef3;
            border: none;
            border-right: 1px solid #d8dee5;
            padding: 5px;
            font-weight: 600;
        }
        QFrame#counterCard {
            background: #eef5fc;
            border: 1px solid #cfe1f3;
            border-radius: 6px;
        }
        QLabel#caption { color: #607284; font-size: 9pt; }
        QLabel#counterValue {
            color: #124f88; font-size: 17pt; font-weight: 700;
        }
        QLabel#metricValue {
            font-size: 11pt; font-weight: 600; color: #243746;
        }
        QLabel#otaState {
            background: #f6f8fa; border: 1px solid #d8dee5;
            border-radius: 6px; padding: 8px; min-height: 32px;
        }
        QLabel#hint { color: #607284; padding: 8px; }
        QPlainTextEdit {
            background: #111820; color: #d7e1ea;
            border: 1px solid #29323b; border-radius: 5px;
            selection-background-color: #345f83;
        }
        QProgressBar {
            border: 1px solid #c8d1db; border-radius: 5px;
            background: #edf1f5; text-align: center;
        }
        QProgressBar::chunk {
            background: #2e8b57; border-radius: 4px;
        }
        """
    )

    window = HookMqttTester()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
