from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import hashlib
import hmac
import secrets
import struct
import time
from collections.abc import Awaitable, Callable
from typing import Any

from .files import DeviceFiles
from .link import Link
from .protocol import (
    Capability,
    Channel,
    ControlType,
    CrashType,
    EventType,
    Frame,
    FrameDecoder,
    JobState,
    MediaType,
    OtaType,
    ProtocolError,
    TlvTag,
    Transport,
    decode_tlv,
    encode_frame,
    tlv_u8,
    tlv_u16,
    tlv_u32,
    tlv_u64,
)
from .state_machine import SessionEvent, SessionState, session_transition

EventCallback = Callable[[dict[str, Any]], Awaitable[None]]
ProgressCallback = Callable[[dict[str, Any]], Awaitable[None]]
MediaCallback = Callable[[dict[str, Any]], Awaitable[None]]
ReadyCallback = Callable[["DeviceSession"], Awaitable[None]]


async def _discard_media(event: dict[str, Any]) -> None:
    del event


@dataclasses.dataclass(slots=True)
class DeviceInfo:
    device_id: str
    boot_id: int
    session_id: int
    endpoint: str
    transport: int
    project_name: str
    app_version: str
    idf_version: str
    firmware_sha256: str
    reset_reason: int
    capabilities: int
    auth_mode: int
    max_payload: int

    def as_dict(self) -> dict[str, Any]:
        result = dataclasses.asdict(self)
        bits = {
            0: "log",
            1: "events",
            2: "status",
            3: "time_sync",
            4: "screen",
            5: "ota",
            6: "crash",
            7: "auth",
            8: "rpc",
            9: "jobs",
            10: "image",
            11: "audio",
            12: "mirror",
            13: "files",
            14: "ota_project_name_match",
        }
        names = [name for bit, name in bits.items() if self.capabilities & (1 << bit)]
        names.append("restart")
        if "rpc" in names:
            names.append("input")
        result["capability_names"] = names
        result["ota_project_name_match_required"] = bool(
            self.capabilities & Capability.OTA_PROJECT_NAME_MATCH
        )
        return result


class DeviceSession:
    LOG_CREDIT_GRANT = 256 * 1024
    LOG_CREDIT_LOW_WATER = 128 * 1024

    def __init__(
        self,
        link: Link,
        on_ready: ReadyCallback,
        on_event: EventCallback,
        *,
        on_media: MediaCallback | None = None,
        pairing_token: str | bytes | None = None,
        clock_sync_interval: float = 30.0,
        clock_sync_timeout: float = 3.0,
    ) -> None:
        if clock_sync_interval <= 0 or clock_sync_timeout <= 0:
            raise ValueError("clock sync interval and timeout must be positive")
        self.link = link
        self.info: DeviceInfo | None = None
        self._on_ready = on_ready
        self._on_event = on_event
        self._on_media = on_media or _discard_media
        if isinstance(pairing_token, str):
            try:
                pairing_token = bytes.fromhex(pairing_token)
            except ValueError as exc:
                raise ValueError("pairing token must be 64 hex characters") from exc
        if pairing_token is not None and len(pairing_token) != 32:
            raise ValueError("pairing token must contain 32 bytes")
        self._pairing_token = pairing_token
        self._clock_sync_interval = clock_sync_interval
        self._clock_sync_timeout = clock_sync_timeout
        self._decoder = FrameDecoder()
        self.state = SessionState.NEGOTIATING
        self._write_lock = asyncio.Lock()
        self._request_lock = asyncio.Lock()
        self._pending: dict[int, asyncio.Future[Frame]] = {}
        self._request_id = 0
        channel_count = max(int(channel) for channel in Channel) + 1
        self._sequence = [0] * channel_count
        self._last_rx_sequence: list[int | None] = [None] * channel_count
        self._closed = False
        self._ready = asyncio.Event()
        self._clock_task: asyncio.Task[None] | None = None
        self._log_credit = 0
        self._crash_chunk_max = 1024
        self._ready_announced = False
        self._media_credit = [0] * channel_count
        self.files = DeviceFiles(self)
        self.clock_offset_us: float | None = None
        self.clock_uncertainty_us: float | None = None

    async def run(self) -> None:
        try:
            while not self._closed:
                data = await self.link.read()
                if not data:
                    raise ConnectionError(f"ESP-Iris link closed: {self.link.endpoint}")
                for frame in self._decoder.feed(data):
                    await self._handle_frame(frame, time.monotonic_ns())
        finally:
            self._closed = True
            if self.state is not SessionState.CLOSED:
                self.state = session_transition(self.state, SessionEvent.CLOSE)
            if self._clock_task is not None:
                self._clock_task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await self._clock_task
            for future in self._pending.values():
                if not future.done():
                    future.set_exception(ConnectionError("ESP-Iris session closed"))
            self._pending.clear()
            await self.link.close()

    async def close(self) -> None:
        self._closed = True
        if self.state is not SessionState.CLOSED:
            self.state = session_transition(self.state, SessionEvent.CLOSE)
        await self.link.close()

    async def wait_ready(self, timeout: float = 5.0) -> DeviceInfo:
        await asyncio.wait_for(self._ready.wait(), timeout)
        assert self.info is not None
        return self.info

    def _next_request_id(self) -> int:
        self._request_id = (self._request_id + 1) & 0xFFFFFFFF
        if self._request_id == 0:
            self._request_id = 1
        return self._request_id

    async def _send(
        self,
        channel: int,
        type_: int,
        payload: bytes = b"",
        *,
        flags: int = 0,
        request_id: int = 0,
        stream_id: int = 0,
    ) -> None:
        if self.info is None:
            session_id = 0
        else:
            session_id = self.info.session_id
        self._sequence[int(channel)] += 1
        frame = Frame(
            channel=channel,
            type=type_,
            flags=flags,
            session_id=session_id,
            request_id=request_id,
            stream_id=stream_id,
            sequence=self._sequence[int(channel)],
            payload=payload,
        )
        async with self._write_lock:
            await self.link.write(encode_frame(frame))

    async def _request(
        self,
        channel: int,
        type_: int,
        payload: bytes = b"",
        timeout: float = 3.0,
        *,
        stream_id: int = 0,
    ) -> Frame:
        async with self._request_lock:
            await self.wait_ready(timeout)
            request_id = self._next_request_id()
            future = asyncio.get_running_loop().create_future()
            self._pending[request_id] = future
            try:
                await self._send(
                    channel,
                    type_,
                    payload,
                    request_id=request_id,
                    stream_id=stream_id,
                )
                return await asyncio.wait_for(future, timeout)
            finally:
                self._pending.pop(request_id, None)

    @staticmethod
    def _text(fields: dict[int, bytes], tag: TlvTag) -> str:
        return fields.get(int(tag), b"").decode("utf-8", errors="replace")

    async def _handle_hello(self, frame: Frame) -> None:
        fields = decode_tlv(frame.payload)
        raw_device_id = fields.get(int(TlvTag.DEVICE_ID), b"")
        if len(raw_device_id) != 16:
            raise ProtocolError("HELLO is missing a 16-byte device ID")
        protocol_version = tlv_u16(fields, TlvTag.PROTOCOL_VERSION)
        if protocol_version != 1:
            raise ProtocolError(f"unsupported ESP-Iris protocol {protocol_version}")

        info = DeviceInfo(
            device_id=raw_device_id.hex(),
            boot_id=tlv_u64(fields, TlvTag.BOOT_ID),
            session_id=frame.session_id,
            endpoint=self.link.endpoint,
            transport=tlv_u8(fields, TlvTag.TRANSPORT),
            project_name=self._text(fields, TlvTag.PROJECT_NAME),
            app_version=self._text(fields, TlvTag.APP_VERSION),
            idf_version=self._text(fields, TlvTag.IDF_VERSION),
            firmware_sha256=fields.get(int(TlvTag.FIRMWARE_SHA256), b"").hex(),
            reset_reason=tlv_u32(fields, TlvTag.RESET_REASON),
            capabilities=tlv_u64(fields, TlvTag.CAPABILITIES),
            auth_mode=tlv_u8(fields, TlvTag.AUTH_MODE),
            max_payload=tlv_u32(fields, TlvTag.MAX_PAYLOAD, 4000),
        )
        if self.info is not None and (
            self.info.device_id != info.device_id
            or self.info.session_id != info.session_id
        ):
            raise ProtocolError("device identity/session changed on a live link")
        self.info = info
        if info.auth_mode == 0:
            await self._send(Channel.CONTROL, ControlType.HELLO_ACK)
            await self._complete_ready()
            return
        if info.auth_mode != 1:
            raise ProtocolError(f"unsupported ESP-Iris auth mode {info.auth_mode}")
        challenge = fields.get(int(TlvTag.AUTH_CHALLENGE), b"")
        if len(challenge) != 32:
            raise ProtocolError("authenticated HELLO has no 32-byte challenge")
        if self._pairing_token is None:
            raise ProtocolError("device requires a pairing token")
        nonce = secrets.token_bytes(16)
        message = (
            b"ESP-Iris-auth-v1"
            + raw_device_id
            + struct.pack("<QI", info.boot_id, info.session_id)
            + challenge
            + nonce
        )
        proof = hmac.new(self._pairing_token, message, hashlib.sha256).digest()
        await self._send(
            Channel.CONTROL, ControlType.HELLO_ACK, nonce + proof
        )

    async def _complete_ready(self) -> None:
        if self._ready_announced:
            return
        assert self.info is not None
        self.state = session_transition(
            self.state, SessionEvent.AUTHENTICATED
        )
        if self._log_credit == 0:
            await self._grant_log_credit(self.LOG_CREDIT_GRANT)
        await self._on_ready(self)
        self._ready_announced = True
        self._ready.set()
        self._clock_task = asyncio.create_task(
            self._clock_loop(), name=f"iris-clock-{self.info.device_id}"
        )

    async def _grant_log_credit(self, amount: int) -> None:
        payload = bytes([int(Channel.LOG), 0, 0, 0]) + struct.pack("<I", amount)
        await self._send(Channel.CONTROL, ControlType.CREDIT, payload)
        self._log_credit += amount

    async def _handle_log(self, frame: Frame) -> None:
        if len(frame.payload) < 16:
            raise ProtocolError("short LOG record")
        monotonic_us, dropped, source, flags, length = struct.unpack_from(
            "<QIBBH", frame.payload
        )
        if len(frame.payload) != 16 + length:
            raise ProtocolError("LOG record length mismatch")
        data = frame.payload[16:]
        self._log_credit = max(0, self._log_credit - len(frame.payload))
        event: dict[str, Any] = {
            "kind": "log",
            "device_id": self.info.device_id if self.info else None,
            "boot_id": self.info.boot_id if self.info else None,
            "monotonic_us": monotonic_us,
            "estimated_wall_ns": self.estimate_wall_ns(monotonic_us),
            "dropped_bytes": dropped,
            "source": "stderr" if source == 2 else "stdout",
            "flags": flags,
            "text": data.decode("utf-8", errors="replace"),
        }
        await self._on_event(event)
        if self._log_credit < self.LOG_CREDIT_LOW_WATER:
            await self._grant_log_credit(self.LOG_CREDIT_GRANT)

    async def _handle_event(self, frame: Frame, host_receive_ns: int) -> None:
        if frame.type == EventType.JOB_UPDATE:
            job = self._decode_job(frame.payload)
            await self._on_event(
                {
                    "kind": "job",
                    "device_id": self.info.device_id if self.info else None,
                    "boot_id": self.info.boot_id if self.info else None,
                    "session_id": self.info.session_id if self.info else None,
                    "host_receive_monotonic_ns": host_receive_ns,
                    "host_receive_wall_ns": time.time_ns(),
                    **job,
                }
            )
            return
        fields = decode_tlv(frame.payload)
        monotonic_us = tlv_u64(fields, TlvTag.UPTIME_US)
        boot_id = tlv_u64(
            fields, TlvTag.BOOT_ID, self.info.boot_id if self.info else 0
        )
        await self._on_event(
            {
                "kind": "device_event",
                "event_type": frame.type,
                "event_name": EventType(frame.type).name.lower()
                if frame.type in EventType._value2member_map_
                else "unknown",
                "device_id": self.info.device_id if self.info else None,
                "boot_id": boot_id,
                "session_id": self.info.session_id if self.info else None,
                "monotonic_us": monotonic_us,
                "estimated_wall_ns": self.estimate_wall_ns(monotonic_us),
                "host_receive_monotonic_ns": host_receive_ns,
                "host_receive_wall_ns": time.time_ns(),
                "clock_uncertainty_us": self.clock_uncertainty_us,
                "reset_reason": tlv_u32(fields, TlvTag.RESET_REASON),
                "sequence": frame.sequence,
                "event_id": (
                    f"{self.info.device_id if self.info else 'unknown'}:"
                    f"{boot_id}:{monotonic_us}:{frame.sequence}"
                ),
            }
        )

    @staticmethod
    def _decode_job(payload: bytes) -> dict[str, Any]:
        if len(payload) != 16:
            raise ProtocolError("invalid job status size")
        job_id, kind, state, cancelled, progress, reserved, result = (
            struct.unpack("<IHBBHHi", payload)
        )
        if reserved != 0:
            raise ProtocolError("job status reserved field is nonzero")
        return {
            "job_id": job_id,
            "job_kind": kind,
            "job_state": JobState(state).name.lower()
            if state in JobState._value2member_map_
            else "unknown",
            "job_state_code": state,
            "cancel_requested": bool(cancelled),
            "progress_permille": progress,
            "result": result,
        }

    async def _grant_media_credit(self, channel: int, amount: int) -> None:
        payload = bytes([int(channel), 0, 0, 0]) + struct.pack("<I", amount)
        await self._send(Channel.CONTROL, ControlType.CREDIT, payload)
        self._media_credit[int(channel)] += amount

    async def _handle_media(self, frame: Frame) -> None:
        if frame.type != MediaType.DATA or len(frame.payload) < 36:
            raise ProtocolError("invalid media data frame")
        (
            monotonic_us,
            frame_id,
            dropped,
            flags,
            data_size,
            x,
            y,
            width,
            height,
            stride,
            format_,
            quality,
        ) = struct.unpack_from("<QIIHHHHHHIHH", frame.payload)
        if len(frame.payload) != 36 + data_size:
            raise ProtocolError("media data size mismatch")
        self._media_credit[int(frame.channel)] = max(
            0, self._media_credit[int(frame.channel)] - len(frame.payload)
        )
        await self._on_media(
            {
                "kind": "media",
                "device_id": self.info.device_id if self.info else None,
                "boot_id": self.info.boot_id if self.info else None,
                "session_id": self.info.session_id if self.info else None,
                "channel": int(frame.channel),
                "stream_id": frame.stream_id,
                "frame_id": frame_id,
                "monotonic_us": monotonic_us,
                "estimated_wall_ns": self.estimate_wall_ns(monotonic_us),
                "dropped": dropped,
                "flags": flags,
                "description": {
                    "x": x,
                    "y": y,
                    "width": width,
                    "height": height,
                    "stride": stride,
                    "format": format_,
                    "quality": quality,
                },
                "data": frame.payload[36:],
            }
        )
        if self._media_credit[int(frame.channel)] < 64 * 1024:
            await self._grant_media_credit(frame.channel, 128 * 1024)

    def _accept_sequence(self, frame: Frame) -> bool:
        channel = int(frame.channel)
        last = self._last_rx_sequence[channel]
        if last is None:
            self._last_rx_sequence[channel] = frame.sequence
            return True
        distance = (frame.sequence - last) & 0xFFFFFFFF
        if distance == 0 or distance >= 0x80000000:
            return False
        self._last_rx_sequence[channel] = frame.sequence
        return True

    async def _handle_frame(self, frame: Frame, host_receive_ns: int) -> None:
        if frame.channel == Channel.CONTROL and frame.type == ControlType.HELLO:
            await self._handle_hello(frame)
            return
        if self.info is None or frame.session_id != self.info.session_id:
            return
        if not self._accept_sequence(frame):
            return
        if (
            frame.channel == Channel.CONTROL
            and frame.type == ControlType.AUTH_RESULT
        ):
            if frame.payload != b"\x01" or frame.flags & 0x02:
                raise ProtocolError("ESP-Iris pairing proof was rejected")
            await self._complete_ready()
            return
        if frame.request_id and frame.request_id in self._pending:
            future = self._pending[frame.request_id]
            if not future.done():
                if frame.type == ControlType.ERROR:
                    code = struct.unpack_from("<I", frame.payload + b"\0\0\0\0")[0]
                    future.set_exception(RuntimeError(f"device error 0x{code:08x}"))
                else:
                    future.set_result(frame)
            return
        if frame.channel == Channel.LOG and frame.type == 1:
            await self._handle_log(frame)
        elif frame.channel == Channel.EVENT:
            await self._handle_event(frame, host_receive_ns)
        elif frame.channel in (Channel.SCREEN, Channel.IMAGE, Channel.AUDIO):
            await self._handle_media(frame)

    async def status(self) -> dict[str, Any]:
        frame = await self._request(Channel.CONTROL, ControlType.STATUS_REQUEST)
        if frame.type != ControlType.STATUS_RESPONSE:
            raise ProtocolError("unexpected status response")
        fields = decode_tlv(frame.payload)
        assert self.info is not None
        return {
            **self.info.as_dict(),
            "uptime_us": tlv_u64(fields, TlvTag.UPTIME_US),
            "free_internal": tlv_u32(fields, TlvTag.FREE_INTERNAL),
            "min_free_internal": tlv_u32(fields, TlvTag.MIN_FREE_INTERNAL),
            "log_dropped_bytes": tlv_u32(fields, TlvTag.LOG_DROPPED),
            "rx_frames": tlv_u32(fields, TlvTag.RX_FRAMES),
            "tx_frames": tlv_u32(fields, TlvTag.TX_FRAMES),
            "invalid_frames": tlv_u32(fields, TlvTag.INVALID_FRAMES),
            "link_count": tlv_u32(fields, TlvTag.LINK_COUNT),
            "task_stack_free_min_bytes": tlv_u32(
                fields, TlvTag.TASK_STACK_FREE_MIN
            ),
            "worker_active_max_us": tlv_u32(
                fields, TlvTag.WORKER_ACTIVE_MAX_US
            ),
            "lifecycle_state": tlv_u8(fields, TlvTag.LIFECYCLE_STATE),
            "internal_heap_used_bytes": tlv_u32(
                fields, TlvTag.INTERNAL_HEAP_USED
            ),
            "static_internal_bytes": tlv_u32(
                fields, TlvTag.STATIC_INTERNAL_BYTES
            ),
            "internal_total_bytes": (
                tlv_u32(fields, TlvTag.STATIC_INTERNAL_BYTES)
                + tlv_u32(fields, TlvTag.INTERNAL_HEAP_USED)
            ),
            "clock_offset_us": self.clock_offset_us,
            "clock_uncertainty_us": self.clock_uncertainty_us,
        }

    async def rpc(
        self,
        service_id: int,
        method_id: int,
        payload: bytes = b"",
        *,
        deadline_ms: int = 1000,
        timeout: float = 3.0,
    ) -> bytes:
        if (
            not 1 <= service_id <= 0xFFFF
            or not 1 <= method_id <= 0xFFFF
            or not 0 <= deadline_ms <= 0xFFFFFFFF
            or len(payload) > 1024
        ):
            raise ValueError("invalid RPC request")
        request = struct.pack(
            "<HHIHH", service_id, method_id, deadline_ms, len(payload), 0
        ) + payload
        frame = await self._request(
            Channel.CONTROL, ControlType.REQUEST, request, timeout
        )
        if frame.type != ControlType.RESPONSE or len(frame.payload) < 12:
            raise ProtocolError("unexpected RPC response")
        returned_service, returned_method, result, size, reserved = (
            struct.unpack_from("<HHiHH", frame.payload)
        )
        if (
            returned_service != service_id
            or returned_method != method_id
            or reserved != 0
            or len(frame.payload) != 12 + size
        ):
            raise ProtocolError("invalid RPC response")
        if result != 0:
            raise RuntimeError(f"RPC failed with device error 0x{result:08x}")
        return frame.payload[12:]

    async def job(self, job_id: int, *, cancel: bool = False) -> dict[str, Any]:
        if not 1 <= job_id <= 0xFFFFFFFF:
            raise ValueError("invalid job ID")
        frame = await self._request(
            Channel.CONTROL,
            ControlType.CANCEL if cancel else ControlType.JOB_QUERY,
            struct.pack("<I", job_id),
        )
        if frame.type != ControlType.JOB_STATUS:
            raise ProtocolError("unexpected job response")
        return self._decode_job(frame.payload)

    @staticmethod
    def _encode_media_description(description: dict[str, int]) -> bytes:
        values = (
            description.get("x", 0),
            description.get("y", 0),
            description.get("width", 0),
            description.get("height", 0),
            description.get("stride", 0),
            description.get("format", 1),
            description.get("quality", 0),
        )
        if any(value < 0 for value in values):
            raise ValueError("media description fields must be nonnegative")
        return struct.pack("<HHHHIHH", *values)

    @staticmethod
    def _decode_media_description(payload: bytes) -> dict[str, int]:
        if len(payload) < 16:
            raise ProtocolError("short media description")
        x, y, width, height, stride, format_, quality = struct.unpack_from(
            "<HHHHIHH", payload
        )
        return {
            "x": x,
            "y": y,
            "width": width,
            "height": height,
            "stride": stride,
            "format": format_,
            "quality": quality,
        }

    async def screenshot(
        self, description: dict[str, int] | None = None
    ) -> tuple[dict[str, int], bytes]:
        requested = description or {}
        frame = await self._request(
            Channel.SCREEN,
            MediaType.OPEN,
            self._encode_media_description(requested),
        )
        if frame.type != MediaType.OPENED or len(frame.payload) != 20:
            raise ProtocolError("unexpected screenshot OPEN response")
        actual = self._decode_media_description(frame.payload)
        total_size = struct.unpack_from("<I", frame.payload, 16)[0]
        result = bytearray()
        try:
            while len(result) < total_size:
                assert self.info is not None
                maximum = min(
                    max(self.info.max_payload - 64, 1),
                    0xFFFF,
                    total_size - len(result),
                )
                chunk_frame = await self._request(
                    Channel.SCREEN,
                    MediaType.READ,
                    struct.pack("<IHH", len(result), maximum, 0),
                )
                if chunk_frame.type != MediaType.DATA or len(chunk_frame.payload) < 8:
                    raise ProtocolError("unexpected screenshot DATA response")
                offset, returned_total = struct.unpack_from(
                    "<II", chunk_frame.payload
                )
                chunk = chunk_frame.payload[8:]
                if (
                    offset != len(result)
                    or returned_total != total_size
                    or not chunk
                ):
                    raise ProtocolError("invalid screenshot chunk")
                result.extend(chunk)
        finally:
            with contextlib.suppress(Exception):
                await self._request(Channel.SCREEN, MediaType.CLOSE)
        return actual, bytes(result)

    async def mirror_start(
        self,
        channel: int,
        description: dict[str, int] | None = None,
        *,
        fps: int = 5,
    ) -> dict[str, Any]:
        if channel not in (Channel.SCREEN, Channel.IMAGE, Channel.AUDIO):
            raise ValueError("invalid media channel")
        if not 1 <= fps <= 60:
            raise ValueError("mirror FPS must be between 1 and 60")
        payload = self._encode_media_description(description or {}) + struct.pack(
            "<HH", fps, 0
        )
        frame = await self._request(channel, MediaType.MIRROR_START, payload)
        if frame.type != MediaType.MIRROR_STATE:
            raise ProtocolError("unexpected mirror start response")
        await self._grant_media_credit(channel, 128 * 1024)
        return {
            "channel": int(channel),
            "stream_id": frame.stream_id,
            "fps": fps,
            "description": self._decode_media_description(frame.payload),
        }

    async def mirror_stop(self, channel: int) -> None:
        if channel not in (Channel.SCREEN, Channel.IMAGE, Channel.AUDIO):
            raise ValueError("invalid media channel")
        frame = await self._request(channel, MediaType.MIRROR_STOP)
        if frame.type != MediaType.MIRROR_STATE:
            raise ProtocolError("unexpected mirror stop response")
        self._media_credit[int(channel)] = 0

    async def ota_update(
        self,
        image: bytes,
        *,
        expected_sha256: bytes | None = None,
        project_name: str = "",
        version: str = "",
        timeout: float = 10.0,
        progress_callback: ProgressCallback | None = None,
    ) -> dict[str, Any]:
        if not image:
            raise ValueError("OTA image is empty")
        digest = expected_sha256 or hashlib.sha256(image).digest()
        if len(digest) != 32:
            raise ValueError("OTA SHA-256 must contain 32 bytes")
        project = project_name.encode()
        release = version.encode()
        if len(project) > 32 or len(release) > 32:
            raise ValueError("OTA project/version is too long")
        begin = (
            struct.pack("<I", len(image))
            + digest
            + bytes([len(project), len(release)])
            + b"\0\0"
            + project
            + release
        )
        frame = await self._request(
            Channel.OTA, OtaType.BEGIN, begin, timeout
        )
        if frame.type != OtaType.BEGIN_RESPONSE or len(frame.payload) < 11:
            raise ProtocolError("unexpected OTA begin response")
        job_id, total_size, chunk_size = struct.unpack_from(
            "<IIH", frame.payload
        )
        label_size = frame.payload[10]
        if (
            total_size != len(image)
            or chunk_size == 0
            or len(frame.payload) != 11 + label_size
        ):
            raise ProtocolError("invalid OTA begin response")
        partition = frame.payload[11:].decode("ascii", errors="replace")
        if progress_callback is not None:
            await progress_callback(
                {
                    "stage": "transferring",
                    "job_id": job_id,
                    "bytes_received": 0,
                    "bytes_total": len(image),
                    "progress_permille": 0,
                    "partition": partition,
                }
            )
        offset = 0
        end_confirmed_by_job = False
        try:
            while offset < len(image):
                chunk = image[offset : offset + chunk_size]
                try:
                    data_response = await self._request(
                        Channel.OTA,
                        OtaType.DATA,
                        struct.pack("<I", offset) + chunk,
                        timeout,
                    )
                except TimeoutError:
                    status = await self.ota_status(timeout=timeout)
                    if not status["active"] or status["job_id"] != job_id:
                        raise TimeoutError(
                            f"OTA data response timed out; device status is {status}"
                        ) from None
                    received = int(status["bytes_received"])
                    if received not in (offset, offset + len(chunk)):
                        raise ProtocolError(
                            f"OTA resumed at unexpected byte offset {received}"
                        )
                    offset = received
                    if progress_callback is not None:
                        await progress_callback(status)
                    continue
                if (
                    data_response.type != OtaType.DATA_RESPONSE
                    or len(data_response.payload) != 8
                ):
                    raise ProtocolError("unexpected OTA data response")
                received, progress, reserved = struct.unpack(
                    "<IHH", data_response.payload
                )
                if received != offset + len(chunk) or reserved != 0:
                    raise ProtocolError("invalid OTA progress")
                offset = received
                if progress_callback is not None:
                    await progress_callback(
                        {
                            "stage": "transferring",
                            "job_id": job_id,
                            "bytes_received": received,
                            "bytes_total": len(image),
                            "progress_permille": progress,
                            "partition": partition,
                        }
                    )
            if progress_callback is not None:
                await progress_callback(
                    {
                        "stage": "verifying",
                        "job_id": job_id,
                        "bytes_received": len(image),
                        "bytes_total": len(image),
                        "progress_permille": 950,
                        "partition": partition,
                    }
                )
            try:
                end = await self._request(Channel.OTA, OtaType.END, b"", timeout)
            except TimeoutError:
                job_status = await self.job(job_id)
                if (
                    job_status.get("job_state") != "succeeded"
                    or int(job_status.get("result", -1)) != 0
                ):
                    raise TimeoutError(
                        f"OTA end response timed out; device Job is {job_status}"
                    ) from None
                end_confirmed_by_job = True
                end = None
        except BaseException:
            with contextlib.suppress(Exception):
                await self._request(Channel.OTA, OtaType.CANCEL, b"", timeout)
            raise
        if not end_confirmed_by_job:
            if end is None or end.type != OtaType.END_RESPONSE or len(end.payload) != 8:
                raise ProtocolError("unexpected OTA end response")
            returned_job, result = struct.unpack("<Ii", end.payload)
            if returned_job != job_id or result != 0:
                raise RuntimeError(f"OTA failed with device error 0x{result:08x}")
        return {
            "job_id": job_id,
            "bytes": len(image),
            "sha256": digest.hex(),
            "partition": partition,
            "restart_required": True,
            "completion_evidence": "device_job" if end_confirmed_by_job else "end_response",
        }

    async def ota_status(self, *, timeout: float = 10.0) -> dict[str, Any]:
        frame = await self._request(Channel.OTA, OtaType.STATUS, b"", timeout)
        if frame.type != OtaType.STATUS or len(frame.payload) < 20:
            raise ProtocolError("unexpected OTA status response")
        job_id, total, received, progress = struct.unpack_from(
            "<IIIH", frame.payload
        )
        active = bool(frame.payload[14])
        label_size = frame.payload[15]
        result = struct.unpack_from("<i", frame.payload, 16)[0]
        if len(frame.payload) != 20 + label_size:
            raise ProtocolError("invalid OTA status response")
        return {
            "stage": "transferring" if active else "idle",
            "job_id": job_id,
            "bytes_total": total,
            "bytes_received": received,
            "progress_permille": progress,
            "active": active,
            "result": result,
            "partition": frame.payload[20:].decode("ascii", errors="replace"),
        }

    async def restart(self, delay_ms: int = 250) -> int:
        if not 100 <= delay_ms <= 60000:
            raise ValueError("restart delay must be 100..60000 ms")
        frame = await self._request(
            Channel.CONTROL,
            ControlType.RESTART,
            struct.pack("<I", delay_ms),
        )
        if frame.type != ControlType.RESTART or len(frame.payload) != 4:
            raise ProtocolError("unexpected restart response")
        return struct.unpack("<I", frame.payload)[0]

    async def sync_clock(self, timeout: float = 3.0) -> None:
        t1_ns = time.monotonic_ns()
        frame = await self._request(
            Channel.CONTROL,
            ControlType.TIME_SYNC_REQUEST,
            struct.pack("<Q", t1_ns),
            timeout,
        )
        t4_ns = time.monotonic_ns()
        if frame.type != ControlType.TIME_SYNC_RESPONSE or len(frame.payload) != 24:
            raise ProtocolError("unexpected time sync response")
        echoed_t1, d2_us, d3_us = struct.unpack("<QQQ", frame.payload)
        if echoed_t1 != t1_ns:
            raise ProtocolError("time sync request echo mismatch")
        host_mid_us = ((t1_ns + t4_ns) / 2.0) / 1000.0
        device_mid_us = (d2_us + d3_us) / 2.0
        rtt_us = max(0.0, (t4_ns - t1_ns) / 1000.0 - (d3_us - d2_us))
        if (
            self.clock_uncertainty_us is None
            or rtt_us / 2.0 < self.clock_uncertainty_us
        ):
            self.clock_offset_us = host_mid_us - device_mid_us
            self.clock_uncertainty_us = rtt_us / 2.0

    async def _clock_loop(self) -> None:
        while not self._closed:
            try:
                await self.sync_clock(self._clock_sync_timeout)
            except (TimeoutError, ConnectionError, ProtocolError, RuntimeError):
                if (
                    self.info is not None
                    and self.info.transport == Transport.USB_SERIAL_JTAG
                ):
                    # USB Serial/JTAG is a fixed hardware endpoint. A physical
                    # disconnect produces EOF/re-enumeration in run(), so a
                    # missed clock probe is not evidence of a stale endpoint.
                    # Closing it here can itself pulse the controller's reset
                    # state on some hosts.
                    await asyncio.sleep(self._clock_sync_interval)
                    continue
                # A serial read timeout is handled inside SerialLink and is not
                # an EOF. An unanswered protocol request is different: the
                # enumerated CDC endpoint can outlive the recovery firmware
                # that created it. Close that stale, already-authenticated
                # session so this process never keeps writing to old device
                # state after the board has switched to its normal TCP image.
                await self.link.close()
                return
            await asyncio.sleep(self._clock_sync_interval)

    def estimate_wall_ns(self, device_monotonic_us: int) -> int | None:
        if self.clock_offset_us is None:
            return None
        host_monotonic_us = device_monotonic_us + self.clock_offset_us
        wall_minus_monotonic_ns = time.time_ns() - time.monotonic_ns()
        return int(host_monotonic_us * 1000 + wall_minus_monotonic_ns)

    async def crash_report(self) -> dict[str, Any]:
        frame = await self._request(Channel.CRASH, CrashType.METADATA_REQUEST)
        if frame.channel != Channel.CRASH or frame.type != CrashType.METADATA_RESPONSE:
            raise ProtocolError("unexpected crash metadata response")
        fields = decode_tlv(frame.payload)
        self._crash_chunk_max = tlv_u32(
            fields, TlvTag.CORE_DUMP_CHUNK_MAX, 1024
        )
        core_sha = fields.get(int(TlvTag.CORE_DUMP_ELF_SHA256), b"").decode(
            "ascii", errors="replace"
        )
        assert self.info is not None
        sha_matches = bool(core_sha) and self.info.firmware_sha256.startswith(core_sha)
        sha_complete = bool(tlv_u8(fields, TlvTag.CORE_DUMP_ELF_SHA256_COMPLETE))
        return {
            "device_id": self.info.device_id,
            "boot_id": tlv_u64(fields, TlvTag.BOOT_ID, self.info.boot_id),
            "session_id": self.info.session_id,
            "reset_reason": tlv_u32(fields, TlvTag.RESET_REASON),
            "previous_boot_crash": bool(
                tlv_u8(fields, TlvTag.PREVIOUS_BOOT_CRASH)
            ),
            "core_dump_present": bool(tlv_u8(fields, TlvTag.CORE_DUMP_PRESENT)),
            "core_dump_valid": bool(tlv_u8(fields, TlvTag.CORE_DUMP_VALID)),
            "core_dump_size": tlv_u32(fields, TlvTag.CORE_DUMP_SIZE),
            "core_dump_chunk_max": self._crash_chunk_max,
            "core_dump_elf_sha256": core_sha,
            "core_dump_elf_sha256_complete": sha_complete,
            "firmware_sha256": self.info.firmware_sha256,
            "firmware_sha_matches": sha_matches,
            "decode_eligible": sha_complete and sha_matches,
            "panic_reason": fields.get(int(TlvTag.PANIC_REASON), b"").decode(
                "utf-8", errors="replace"
            ),
        }

    async def read_core_dump_chunk(
        self, offset: int, maximum: int = 1024
    ) -> tuple[int, bytes]:
        if not 0 <= offset <= 0xFFFFFFFF or not 1 <= maximum <= 2048:
            raise ValueError("invalid coredump chunk range")
        maximum = min(maximum, self._crash_chunk_max)
        request = struct.pack("<IHH", offset, maximum, 0)
        frame = await self._request(Channel.CRASH, CrashType.READ_REQUEST, request)
        if frame.channel != Channel.CRASH or frame.type != CrashType.READ_RESPONSE:
            raise ProtocolError("unexpected coredump read response")
        if len(frame.payload) < 8:
            raise ProtocolError("short coredump read response")
        returned_offset, total_size = struct.unpack_from("<II", frame.payload)
        if returned_offset != offset:
            raise ProtocolError("coredump offset mismatch")
        return total_size, frame.payload[8:]
