"""Protocol helpers for the Hook MQTT test workstation.

The MQTT payload is the same 0xA5 binary frame used on the MCU serial link:

    SOF | SEQ | CMD_ID | DATA_LEN | DATA... | CRC16 little-endian

CRC16 is CCITT (polynomial 0x1021, initial value 0x0000).
"""

from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Iterable


SOF = 0xA5

CMD_LIGHT = 0x01
CMD_BMS = 0x02
CMD_HEARTBEAT_EN = 0x04
CMD_HEARTBEAT = 0x05
CMD_WORK_MODE = 0x06
CMD_SLEEP_EN = 0x07
CMD_SLEEP_TIME = 0x08
CMD_CURRENT_TIME = 0x09
CMD_SYS_ERROR = 0xF0

WORK_MODES = {
    0: "待机",
    1: "正常通信",
    2: "低电量",
    3: "设备异常",
}

ERROR_BITS = {
    0: "上位机心跳超时",
    1: "声光报警模块离线",
    2: "BMS 读取失败",
}

PROTECTION_BITS = {
    0: "单节过压",
    1: "单节欠压",
    2: "整组过压",
    3: "整组欠压",
    4: "充电过温",
    5: "充电低温",
    6: "放电过温",
    7: "放电低温",
    8: "充电过流",
    9: "放电过流",
    10: "短路",
    11: "前端检测 IC 错误",
    12: "软件锁定 MOS",
    13: "断线告警",
    14: "二次过压告警",
}

LOCK_BIT = 15


def crc16_ccitt(data: bytes) -> int:
    """Return CRC16-CCITT with polynomial 0x1021 and initial value 0."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


@dataclass(frozen=True)
class Frame:
    seq: int
    cmd_id: int
    data: bytes

    @property
    def is_error(self) -> bool:
        return bool(self.cmd_id & 0x80)

    @property
    def base_cmd_id(self) -> int:
        return self.cmd_id & 0x7F


def build_frame(cmd_id: int, data: bytes = b"", seq: int = 0) -> bytes:
    """Build one 0xA5 frame without any usr.cn channel prefix."""
    if not 0 <= cmd_id <= 0xFF:
        raise ValueError("cmd_id must fit in one byte")
    if not 0 <= seq <= 0xFF:
        raise ValueError("seq must fit in one byte")
    if len(data) > 255:
        raise ValueError("data must not exceed 255 bytes")

    body = struct.pack("<BBBB", SOF, seq, cmd_id, len(data)) + bytes(data)
    crc = crc16_ccitt(body)
    return body + struct.pack("<H", crc)


def strip_channel_prefix(payload: bytes) -> bytes:
    """Remove optional printable usr.cn routing prefix from an MQTT body."""
    if len(payload) >= 2 and payload[1:2] == b"," and 0x30 <= payload[0] <= 0x39:
        return payload[2:]
    return payload


class FrameStreamDecoder:
    """Incrementally decode 0xA5 frames and resynchronise after noise."""

    def __init__(self) -> None:
        self._buffer = bytearray()

    def reset(self) -> None:
        self._buffer.clear()

    def feed(self, payload: bytes) -> list[Frame]:
        self._buffer.extend(strip_channel_prefix(payload))
        frames: list[Frame] = []

        while True:
            start = self._buffer.find(SOF)
            if start < 0:
                self._buffer.clear()
                break
            if start:
                del self._buffer[:start]
            if len(self._buffer) < 4:
                break

            data_len = self._buffer[3]
            total_len = 4 + data_len + 2
            if len(self._buffer) < total_len:
                break

            candidate = bytes(self._buffer[:total_len])
            body = candidate[:-2]
            received_crc = struct.unpack_from("<H", candidate, total_len - 2)[0]
            if crc16_ccitt(body) != received_crc:
                # Drop only SOF and search again. This preserves a valid frame
                # that happens to follow a corrupt/partial candidate.
                del self._buffer[0]
                continue

            _, seq, cmd_id, _ = struct.unpack_from("<BBBB", candidate, 0)
            frames.append(Frame(seq=seq, cmd_id=cmd_id,
                                data=candidate[4:4 + data_len]))
            del self._buffer[:total_len]

        return frames


@dataclass(frozen=True)
class BmsStatus:
    battery_raw: int
    voltage_raw: int
    current_raw: int
    remain_discharge_raw: int
    remain_charge_raw: int
    protect_raw: int

    @property
    def battery_percent(self) -> float | None:
        if self.battery_raw == 0xFFFF:
            return None
        return self.battery_raw / 100.0

    @property
    def voltage_v(self) -> float:
        return self.voltage_raw / 100.0

    @property
    def current_a(self) -> float:
        return self.current_raw / 100.0

    @property
    def protect_labels(self) -> list[str]:
        return [label for bit, label in PROTECTION_BITS.items()
                if self.protect_raw & (1 << bit)]

    @property
    def lock_open(self) -> bool:
        return bool(self.protect_raw & (1 << LOCK_BIT))


def decode_bms(payload: bytes) -> BmsStatus:
    if len(payload) != 12:
        raise ValueError(f"BMS payload must be 12 bytes, got {len(payload)}")
    values = struct.unpack("<6H", payload)
    return BmsStatus(*values)


@dataclass(frozen=True)
class WorkStatus:
    mode: int
    error_code: int

    @property
    def mode_text(self) -> str:
        return WORK_MODES.get(self.mode, f"未知模式({self.mode})")

    @property
    def error_labels(self) -> list[str]:
        return [label for bit, label in ERROR_BITS.items()
                if self.error_code & (1 << bit)]


def decode_work_status(payload: bytes) -> WorkStatus:
    if len(payload) != 2:
        raise ValueError(f"work-status payload must be 2 bytes, got {len(payload)}")
    return WorkStatus(mode=payload[0], error_code=payload[1])


def format_error_bits(error_code: int) -> str:
    labels = [label for bit, label in ERROR_BITS.items()
              if error_code & (1 << bit)]
    return "、".join(labels) if labels else "无"


def format_protection(status: BmsStatus) -> str:
    parts = list(status.protect_labels)
    if status.lock_open:
        parts.append("自锁开关断开")
    return "、".join(parts) if parts else "无"


def hex_dump(data: bytes, limit: int = 96) -> str:
    shown = data[:limit]
    result = shown.hex(" ").upper()
    if len(data) > limit:
        result += f" ... ({len(data)} bytes)"
    return result


def derive_topics(device_no: str) -> dict[str, str]:
    raw = device_no.strip()
    if raw.lower().startswith("ais"):
        raw = raw[3:]
    if not raw:
        raise ValueError("device number must not be empty")
    dev = f"ais{raw}"
    return {
        "command": f"ai_safety/{dev}/command/device_manager/hook",
        "heartbeat": f"ai_safety/{dev}/heartbeat/device_manager/hook",
        "inform": f"ai_safety/{dev}/inform/hook/device_manager",
        "system_cmd": f"ai_safety/{dev}/system/server/hook",
        "system_reply": f"ai_safety/{dev}/system/hook/server",
        "ota_reply": f"ai_safety/{dev}/ota/hook/server",
    }


def self_test() -> None:
    frame = FrameStreamDecoder()
    encoded = build_frame(CMD_BMS, b"", seq=7)
    assert encoded[0] == SOF
    assert frame.feed(b"noise3," + encoded[:3]) == []
    decoded = frame.feed(encoded[3:])
    assert len(decoded) == 1
    assert decoded[0].seq == 7
    assert decoded[0].cmd_id == CMD_BMS
    assert decoded[0].data == b""

    sample = struct.pack("<6H", 8734, 5142, 321, 0xFFFF, 95, 0x8001)
    bms = decode_bms(sample)
    assert abs(bms.battery_percent - 87.34) < 0.001
    assert abs(bms.voltage_v - 51.42) < 0.001
    assert "单节过压" in format_protection(bms)
    assert bms.lock_open

    work = decode_work_status(bytes([3, 0x05]))
    assert work.mode_text == "设备异常"
    assert work.error_labels == ["上位机心跳超时", "BMS 读取失败"]

    topics = derive_topics("999")
    assert topics["command"].startswith("ai_safety/ais999/")


if __name__ == "__main__":
    self_test()
    print("hook_mqtt_protocol self-test: OK")
