from __future__ import annotations

import dataclasses
import enum
import operator
import struct
import zlib
from collections.abc import Iterable

MAGIC = b"IRIS"
VERSION = 1
HEADER_SIZE = 32
MAX_PAYLOAD = 4000
MAX_WIRE_FRAME = 4096
HEADER = struct.Struct("<4sBBBBHHIIIII")


class ProtocolError(ValueError):
    pass


class Channel(enum.IntEnum):
    CONTROL = 0
    LOG = 1
    EVENT = 2
    SCREEN = 3
    IMAGE = 4
    AUDIO = 5
    OTA = 6
    CRASH = 7
    FILE = 8


class Transport(enum.IntEnum):
    USB = 1
    TCP = 2
    USB_SERIAL_JTAG = 3


class ControlType(enum.IntEnum):
    HELLO = 0x01
    HELLO_ACK = 0x02
    PING = 0x03
    PONG = 0x04
    TIME_SYNC_REQUEST = 0x05
    TIME_SYNC_RESPONSE = 0x06
    STATUS_REQUEST = 0x07
    STATUS_RESPONSE = 0x08
    CREDIT = 0x09
    REQUEST = 0x10
    RESPONSE = 0x11
    CANCEL = 0x12
    JOB_QUERY = 0x13
    JOB_STATUS = 0x14
    RESTART = 0x15
    AUTH_RESULT = 0x16
    ERROR = 0x7F


class EventType(enum.IntEnum):
    BOOT = 0x01
    HEALTHY = 0x03
    PLANNED_RESTART = 0x04
    LINK_READY = 0x05
    PREVIOUS_BOOT_CRASH = 0x06
    RECOVERY_ENTERED = 0x07
    CORE_DUMP_AVAILABLE = 0x08
    JOB_UPDATE = 0x20
    OTA_READY = 0x21


class CrashType(enum.IntEnum):
    METADATA_REQUEST = 0x01
    METADATA_RESPONSE = 0x02
    READ_REQUEST = 0x03
    READ_RESPONSE = 0x04


class MediaType(enum.IntEnum):
    OPEN = 0x01
    OPENED = 0x02
    READ = 0x03
    DATA = 0x04
    CLOSE = 0x05
    MIRROR_START = 0x06
    MIRROR_STOP = 0x07
    MIRROR_STATE = 0x08


class OtaType(enum.IntEnum):
    BEGIN = 0x01
    BEGIN_RESPONSE = 0x02
    DATA = 0x03
    DATA_RESPONSE = 0x04
    END = 0x05
    END_RESPONSE = 0x06
    CANCEL = 0x07
    STATUS = 0x08


class FileType(enum.IntEnum):
    VOLUMES_REQUEST = 0x01
    VOLUMES_RESPONSE = 0x02
    STAT_REQUEST = 0x03
    STAT_RESPONSE = 0x04
    LIST_OPEN = 0x05
    LIST_OPENED = 0x06
    LIST_NEXT = 0x07
    LIST_DATA = 0x08
    CLOSE = 0x09
    CLOSE_RESPONSE = 0x0A
    READ_OPEN = 0x0B
    READ_OPENED = 0x0C
    READ = 0x0D
    DATA = 0x0E
    WRITE_OPEN = 0x0F
    WRITE_OPENED = 0x10
    WRITE = 0x11
    WRITE_ACK = 0x12
    COMMIT = 0x13
    COMMIT_RESPONSE = 0x14
    ABORT = 0x15
    ABORT_RESPONSE = 0x16
    MKDIR = 0x17
    MKDIR_RESPONSE = 0x18
    DELETE = 0x19
    DELETE_RESPONSE = 0x1A
    RENAME = 0x1B
    RENAME_RESPONSE = 0x1C
    WRITE_STATUS = 0x1D
    WRITE_STATUS_RESPONSE = 0x1E


class FileStatus(enum.IntEnum):
    OK = 0
    INVALID_ARGUMENT = 1
    NOT_FOUND = 2
    NOT_DIRECTORY = 3
    NOT_FILE = 4
    READ_ONLY = 5
    BUSY = 6
    NO_MEMORY = 7
    IO = 8
    NOT_SUPPORTED = 9
    CONFLICT = 10
    EXISTS = 11
    NOT_EMPTY = 12
    NO_SPACE = 13
    HASH_MISMATCH = 14


class JobState(enum.IntEnum):
    QUEUED = 0
    RUNNING = 1
    SUCCEEDED = 2
    FAILED = 3
    CANCELLED = 4


class TlvTag(enum.IntEnum):
    DEVICE_ID = 0x01
    BOOT_ID = 0x02
    UPTIME_US = 0x03
    PROTOCOL_VERSION = 0x04
    CAPABILITIES = 0x05
    TRANSPORT = 0x06
    AUTH_MODE = 0x07
    RESET_REASON = 0x08
    PROJECT_NAME = 0x09
    APP_VERSION = 0x0A
    FIRMWARE_SHA256 = 0x0B
    IDF_VERSION = 0x0C
    MAX_PAYLOAD = 0x0D
    FREE_INTERNAL = 0x20
    MIN_FREE_INTERNAL = 0x21
    LOG_DROPPED = 0x22
    RX_FRAMES = 0x23
    TX_FRAMES = 0x24
    INVALID_FRAMES = 0x25
    LINK_COUNT = 0x26
    TASK_STACK_FREE_MIN = 0x27
    WORKER_ACTIVE_MAX_US = 0x28
    LIFECYCLE_STATE = 0x29
    INTERNAL_HEAP_USED = 0x2A
    STATIC_INTERNAL_BYTES = 0x2B
    PREVIOUS_BOOT_CRASH = 0x30
    CORE_DUMP_PRESENT = 0x31
    CORE_DUMP_VALID = 0x32
    CORE_DUMP_SIZE = 0x33
    CORE_DUMP_ELF_SHA256 = 0x34
    CORE_DUMP_ELF_SHA256_COMPLETE = 0x35
    PANIC_REASON = 0x36
    CORE_DUMP_CHUNK_MAX = 0x37
    AUTH_CHALLENGE = 0x40
    JOB_ID = 0x41
    JOB_STATE = 0x42
    JOB_PROGRESS = 0x43
    JOB_RESULT = 0x44
    MEDIA_DROPPED = 0x45
    OTA_PARTITION = 0x46


@dataclasses.dataclass(slots=True)
class Frame:
    channel: int
    type: int
    flags: int = 0
    session_id: int = 0
    request_id: int = 0
    stream_id: int = 0
    sequence: int = 0
    payload: bytes = b""


def _wire_uint(name: str, value: int, bits: int) -> int:
    try:
        normalized = operator.index(value)
    except TypeError as exc:
        raise ProtocolError(f"{name} must be an integer") from exc
    if not 0 <= normalized < (1 << bits):
        raise ProtocolError(f"{name} does not fit in u{bits}")
    return normalized


def cobs_encode(data: bytes) -> bytes:
    output = bytearray(b"\x00")
    code_offset = 0
    code = 1
    for value in data:
        if value == 0:
            output[code_offset] = code
            code_offset = len(output)
            output.append(0)
            code = 1
        else:
            output.append(value)
            code += 1
            if code == 0xFF:
                output[code_offset] = code
                code_offset = len(output)
                output.append(0)
                code = 1
    output[code_offset] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    output = bytearray()
    offset = 0
    while offset < len(data):
        code = data[offset]
        offset += 1
        if code == 0:
            raise ProtocolError("zero byte inside COBS frame")
        count = code - 1
        end = offset + count
        if end > len(data):
            raise ProtocolError("truncated COBS block")
        output.extend(data[offset:end])
        offset = end
        if code != 0xFF and offset < len(data):
            output.append(0)
    return bytes(output)


def encode_frame(frame: Frame) -> bytes:
    payload = bytes(frame.payload)
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError(f"payload exceeds {MAX_PAYLOAD} bytes")
    channel = _wire_uint("channel", frame.channel, 8)
    if channel > int(Channel.FILE):
        raise ProtocolError("invalid channel")
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        channel,
        _wire_uint("type", frame.type, 8),
        _wire_uint("flags", frame.flags, 16),
        0,
        _wire_uint("session_id", frame.session_id, 32),
        _wire_uint("request_id", frame.request_id, 32),
        _wire_uint("stream_id", frame.stream_id, 32),
        _wire_uint("sequence", frame.sequence, 32),
        len(payload),
    )
    plain = header + payload
    plain += struct.pack("<I", zlib.crc32(plain) & 0xFFFFFFFF)
    wire = cobs_encode(plain) + b"\x00"
    if len(wire) > MAX_WIRE_FRAME:
        raise ProtocolError("encoded frame exceeds wire buffer")
    return wire


def decode_frame(encoded: bytes) -> Frame:
    if not encoded or len(encoded) >= MAX_WIRE_FRAME:
        raise ProtocolError("invalid encoded frame size")
    plain = cobs_decode(encoded)
    if len(plain) < HEADER_SIZE + 4:
        raise ProtocolError("frame is shorter than header and CRC")
    fields = HEADER.unpack_from(plain)
    (
        magic,
        version,
        header_size,
        channel,
        type_,
        flags,
        reserved,
        session_id,
        request_id,
        stream_id,
        sequence,
        payload_size,
    ) = fields
    if magic != MAGIC or version != VERSION or header_size != HEADER_SIZE:
        raise ProtocolError("unsupported frame header")
    if reserved != 0 or channel > int(Channel.FILE):
        raise ProtocolError("invalid reserved field or channel")
    if payload_size > MAX_PAYLOAD or len(plain) != HEADER_SIZE + payload_size + 4:
        raise ProtocolError("payload size mismatch")
    expected_crc = struct.unpack_from("<I", plain, len(plain) - 4)[0]
    actual_crc = zlib.crc32(plain[:-4]) & 0xFFFFFFFF
    if expected_crc != actual_crc:
        raise ProtocolError("CRC32 mismatch")
    return Frame(
        channel=channel,
        type=type_,
        flags=flags,
        session_id=session_id,
        request_id=request_id,
        stream_id=stream_id,
        sequence=sequence,
        payload=plain[HEADER_SIZE:-4],
    )


class FrameDecoder:
    """Incremental delimiter parser that resynchronizes after invalid bytes."""

    def __init__(self) -> None:
        self._encoded = bytearray()
        self._discarding = False
        self.invalid_frames = 0

    def feed(self, data: bytes) -> list[Frame]:
        frames: list[Frame] = []
        for value in data:
            if value == 0:
                if self._discarding:
                    self._discarding = False
                    self._encoded.clear()
                    continue
                if not self._encoded:
                    continue
                try:
                    frames.append(decode_frame(bytes(self._encoded)))
                except ProtocolError:
                    self.invalid_frames += 1
                self._encoded.clear()
            elif not self._discarding:
                if len(self._encoded) >= MAX_WIRE_FRAME - 1:
                    self.invalid_frames += 1
                    self._discarding = True
                    self._encoded.clear()
                else:
                    self._encoded.append(value)
        return frames


def encode_tlv(items: Iterable[tuple[int, bytes]]) -> bytes:
    output = bytearray()
    for tag, value in items:
        value = bytes(value)
        if len(value) > 0xFFFF:
            raise ProtocolError("TLV value is too large")
        output.extend(struct.pack("<BH", int(tag), len(value)))
        output.extend(value)
    return bytes(output)


def decode_tlv(payload: bytes) -> dict[int, bytes]:
    result: dict[int, bytes] = {}
    offset = 0
    while offset < len(payload):
        if len(payload) - offset < 3:
            raise ProtocolError("truncated TLV header")
        tag, length = struct.unpack_from("<BH", payload, offset)
        offset += 3
        end = offset + length
        if end > len(payload):
            raise ProtocolError("truncated TLV value")
        result[tag] = payload[offset:end]
        offset = end
    return result


def tlv_u8(fields: dict[int, bytes], tag: int, default: int = 0) -> int:
    value = fields.get(int(tag))
    return value[0] if value is not None and len(value) == 1 else default


def tlv_u16(fields: dict[int, bytes], tag: int, default: int = 0) -> int:
    value = fields.get(int(tag))
    return (
        struct.unpack("<H", value)[0]
        if value is not None and len(value) == 2
        else default
    )


def tlv_u32(fields: dict[int, bytes], tag: int, default: int = 0) -> int:
    value = fields.get(int(tag))
    return (
        struct.unpack("<I", value)[0]
        if value is not None and len(value) == 4
        else default
    )


def tlv_u64(fields: dict[int, bytes], tag: int, default: int = 0) -> int:
    value = fields.get(int(tag))
    return (
        struct.unpack("<Q", value)[0]
        if value is not None and len(value) == 8
        else default
    )
