import asyncio
import contextlib
import hashlib
import hmac
import struct

import pytest

from iris_gateway.protocol import (
    Channel,
    ControlType,
    CrashType,
    EventType,
    Frame,
    MediaType,
    OtaType,
    ProtocolError,
    TlvTag,
    decode_frame,
    encode_frame,
    encode_tlv,
)
from iris_gateway.session import DeviceSession


class FakeLink:
    endpoint = "fake:test"

    def __init__(self) -> None:
        self.incoming: asyncio.Queue[bytes] = asyncio.Queue()
        self.writes: list[bytes] = []
        self.closed = False

    async def read(self, size: int = 4096) -> bytes:
        return await self.incoming.get()

    async def write(self, data: bytes) -> None:
        self.writes.append(data)

    async def close(self) -> None:
        self.closed = True


async def wait_for_request(
    link: FakeLink, channel: int, type_: int, after: int = 0
) -> tuple[int, Frame]:
    for _ in range(100):
        for index, wire in enumerate(link.writes[after:], start=after):
            frame = decode_frame(wire[:-1])
            if frame.channel == channel and frame.type == type_:
                return index, frame
        await asyncio.sleep(0)
    raise AssertionError(f"request {channel}/{type_} was not sent")


def test_session_hello_credit_and_log_event() -> None:
    async def scenario() -> None:
        link = FakeLink()
        ready: list[str] = []
        events: list[dict[str, object]] = []

        async def on_ready(session: DeviceSession) -> None:
            assert session.info is not None
            ready.append(session.info.device_id)

        async def on_event(event: dict[str, object]) -> None:
            events.append(event)

        session = DeviceSession(link, on_ready, on_event)
        task = asyncio.create_task(session.run())
        hello = encode_tlv(
            [
                (TlvTag.DEVICE_ID, bytes.fromhex("00112233445566778899aabbccddeeff")),
                (TlvTag.BOOT_ID, struct.pack("<Q", 7)),
                (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 1)),
                (TlvTag.CAPABILITIES, struct.pack("<Q", 0x400F)),
                (TlvTag.TRANSPORT, b"\x01"),
                (TlvTag.AUTH_MODE, b"\x00"),
                (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
            ]
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x12345678,
                    sequence=1,
                    payload=hello,
                )
            )
        )
        info = await session.wait_ready()
        assert info.device_id == "00112233445566778899aabbccddeeff"
        assert info.as_dict()["ota_project_name_match_required"] is True
        assert "ota_project_name_match" in info.as_dict()["capability_names"]
        assert ready == [info.device_id]
        sent_types = [decode_frame(wire[:-1]).type for wire in link.writes]
        assert sent_types[:2] == [ControlType.HELLO_ACK, ControlType.CREDIT]

        writes_before_duplicate = len(link.writes)
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=info.session_id,
                    sequence=2,
                    payload=hello,
                )
            )
        )
        for _ in range(20):
            if len(link.writes) > writes_before_duplicate:
                break
            await asyncio.sleep(0)
        duplicate_types = [decode_frame(wire[:-1]).type for wire in link.writes]
        assert ready == [info.device_id]
        assert duplicate_types.count(ControlType.HELLO_ACK) == 2
        assert duplicate_types.count(ControlType.CREDIT) == 1

        log_payload = struct.pack("<QIBBH", 1234, 0, 1, 0, 5) + b"hello"
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.LOG,
                    type=1,
                    session_id=info.session_id,
                    sequence=1,
                    payload=log_payload,
                )
            )
        )
        for _ in range(20):
            if events:
                break
            await asyncio.sleep(0)
        assert events[0]["text"] == "hello"
        assert events[0]["monotonic_us"] == 1234

        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_ota_end_timeout_is_reconciled_with_device_job() -> None:
    async def scenario() -> None:
        session = object.__new__(DeviceSession)
        calls = 0

        async def request(channel, type_, payload=b"", timeout=10.0):
            nonlocal calls
            del payload, timeout
            calls += 1
            if calls == 1:
                assert (channel, type_) == (Channel.OTA, OtaType.BEGIN)
                return Frame(
                    channel=channel,
                    type=OtaType.BEGIN_RESPONSE,
                    payload=struct.pack("<IIHB", 77, 1, 1, 5) + b"ota_1",
                )
            if calls == 2:
                assert (channel, type_) == (Channel.OTA, OtaType.DATA)
                return Frame(
                    channel=channel,
                    type=OtaType.DATA_RESPONSE,
                    payload=struct.pack("<IHH", 1, 900, 0),
                )
            if calls == 3:
                assert (channel, type_) == (Channel.OTA, OtaType.END)
                raise TimeoutError
            assert (channel, type_) == (Channel.CONTROL, ControlType.JOB_QUERY)
            return Frame(
                channel=channel,
                type=ControlType.JOB_STATUS,
                payload=struct.pack("<IHBBHHi", 77, 0x100, 2, 0, 1000, 0, 0),
            )

        session._request = request
        result = await session.ota_update(b"x", timeout=0.01)
        assert result["completion_evidence"] == "device_job"
        assert result["job_id"] == 77

    asyncio.run(scenario())


def test_ota_end_session_close_is_deferred_to_gateway_reconnect_validation() -> None:
    async def scenario() -> None:
        session = object.__new__(DeviceSession)
        calls = 0

        async def request(channel, type_, payload=b"", timeout=10.0):
            nonlocal calls
            del payload, timeout
            calls += 1
            if calls == 1:
                return Frame(
                    channel=channel,
                    type=OtaType.BEGIN_RESPONSE,
                    payload=struct.pack("<IIHB", 78, 1, 1, 5) + b"ota_1",
                )
            if calls == 2:
                return Frame(
                    channel=channel,
                    type=OtaType.DATA_RESPONSE,
                    payload=struct.pack("<IHH", 1, 900, 0),
                )
            assert (channel, type_) == (Channel.OTA, OtaType.END)
            raise ConnectionError("ESP-Iris session closed")

        session._request = request
        result = await session.ota_update(b"x", timeout=0.01)
        assert result["completion_evidence"] == "session_close"
        assert result["job_id"] == 78

    asyncio.run(scenario())


def test_event_time_fields_crash_metadata_and_chunk_download() -> None:
    async def scenario() -> None:
        link = FakeLink()
        events: list[dict[str, object]] = []

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            events.append(event)

        session = DeviceSession(link, on_ready, on_event)
        run_task = asyncio.create_task(session.run())
        device_id = bytes.fromhex("00112233445566778899aabbccddeeff")
        firmware_sha = bytes.fromhex("ab" * 32)
        hello = encode_tlv(
            [
                (TlvTag.DEVICE_ID, device_id),
                (TlvTag.BOOT_ID, struct.pack("<Q", 77)),
                (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 1)),
                (TlvTag.CAPABILITIES, struct.pack("<Q", 0x4F)),
                (TlvTag.TRANSPORT, b"\x01"),
                (TlvTag.AUTH_MODE, b"\x00"),
                (TlvTag.FIRMWARE_SHA256, firmware_sha),
                (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
            ]
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x12345678,
                    sequence=1,
                    payload=hello,
                )
            )
        )
        info = await session.wait_ready()
        assert session._clock_task is not None
        session._clock_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await session._clock_task
        session._clock_task = None

        event_payload = encode_tlv(
            [
                (TlvTag.BOOT_ID, struct.pack("<Q", info.boot_id)),
                (TlvTag.UPTIME_US, struct.pack("<Q", 12345)),
                (TlvTag.RESET_REASON, struct.pack("<I", 9)),
            ]
        )
        event_wire = encode_frame(
            Frame(
                channel=Channel.EVENT,
                type=EventType.BOOT,
                session_id=info.session_id,
                sequence=1,
                payload=event_payload,
            )
        )
        await link.incoming.put(event_wire)
        await link.incoming.put(event_wire)
        for _ in range(20):
            if events:
                break
            await asyncio.sleep(0)
        assert len(events) == 1
        assert events[0]["session_id"] == info.session_id
        assert events[0]["host_receive_monotonic_ns"]
        assert events[0]["host_receive_wall_ns"]
        assert events[0]["event_id"].endswith(":12345:1")

        report_task = asyncio.create_task(session.crash_report())
        for _ in range(20):
            if any(
                decode_frame(wire[:-1]).channel == Channel.CRASH
                for wire in link.writes
            ):
                break
            await asyncio.sleep(0)
        request = next(
            decode_frame(wire[:-1])
            for wire in reversed(link.writes)
            if decode_frame(wire[:-1]).channel == Channel.CRASH
        )
        assert request.type == CrashType.METADATA_REQUEST
        metadata = encode_tlv(
            [
                (TlvTag.BOOT_ID, struct.pack("<Q", info.boot_id)),
                (TlvTag.RESET_REASON, struct.pack("<I", 9)),
                (TlvTag.PREVIOUS_BOOT_CRASH, b"\x01"),
                (TlvTag.CORE_DUMP_PRESENT, b"\x01"),
                (TlvTag.CORE_DUMP_VALID, b"\x01"),
                (TlvTag.CORE_DUMP_SIZE, struct.pack("<I", 4)),
                (TlvTag.CORE_DUMP_CHUNK_MAX, struct.pack("<I", 512)),
                (TlvTag.CORE_DUMP_ELF_SHA256, b"ab" * 32),
                (TlvTag.CORE_DUMP_ELF_SHA256_COMPLETE, b"\x01"),
                (TlvTag.PANIC_REASON, b"Load access fault"),
            ]
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CRASH,
                    type=CrashType.METADATA_RESPONSE,
                    flags=1,
                    session_id=info.session_id,
                    request_id=request.request_id,
                    sequence=1,
                    payload=metadata,
                )
            )
        )
        report = await report_task
        assert report["previous_boot_crash"] is True
        assert report["decode_eligible"] is True
        assert report["core_dump_chunk_max"] == 512
        assert report["panic_reason"] == "Load access fault"

        chunk_task = asyncio.create_task(session.read_core_dump_chunk(0, 1024))
        for _ in range(20):
            crash_requests = [
                decode_frame(wire[:-1])
                for wire in link.writes
                if decode_frame(wire[:-1]).channel == Channel.CRASH
            ]
            if len(crash_requests) >= 2:
                break
            await asyncio.sleep(0)
        chunk_request = crash_requests[-1]
        assert struct.unpack("<IHH", chunk_request.payload) == (0, 512, 0)
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CRASH,
                    type=CrashType.READ_RESPONSE,
                    flags=1 | 16,
                    session_id=info.session_id,
                    request_id=chunk_request.request_id,
                    sequence=2,
                    payload=struct.pack("<II", 0, 4) + b"core",
                )
            )
        )
        assert await chunk_task == (4, b"core")

        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

    asyncio.run(scenario())


def test_pairing_challenge_hmac_gates_session_ready() -> None:
    async def scenario() -> None:
        link = FakeLink()
        token = bytes.fromhex("11" * 32)
        challenge = bytes.fromhex("22" * 32)
        device_id = bytes.fromhex("00112233445566778899aabbccddeeff")
        boot_id = 91
        session_id = 0x87654321

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        session = DeviceSession(
            link, on_ready, on_event, pairing_token=token
        )
        task = asyncio.create_task(session.run())
        hello = encode_tlv(
            [
                (TlvTag.DEVICE_ID, device_id),
                (TlvTag.BOOT_ID, struct.pack("<Q", boot_id)),
                (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 1)),
                (TlvTag.CAPABILITIES, struct.pack("<Q", 0x1FF)),
                (TlvTag.TRANSPORT, b"\x02"),
                (TlvTag.AUTH_MODE, b"\x01"),
                (TlvTag.AUTH_CHALLENGE, challenge),
                (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
            ]
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=session_id,
                    sequence=1,
                    payload=hello,
                )
            )
        )
        _, proof_frame = await wait_for_request(
            link, Channel.CONTROL, ControlType.HELLO_ACK
        )
        assert len(proof_frame.payload) == 48
        nonce = proof_frame.payload[:16]
        expected = hmac.new(
            token,
            b"ESP-Iris-auth-v1"
            + device_id
            + struct.pack("<QI", boot_id, session_id)
            + challenge
            + nonce,
            hashlib.sha256,
        ).digest()
        assert hmac.compare_digest(proof_frame.payload[16:], expected)
        assert not session._ready.is_set()
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.AUTH_RESULT,
                    flags=1,
                    session_id=session_id,
                    sequence=1,
                    payload=b"\x01",
                )
            )
        )
        info = await session.wait_ready()
        assert info.auth_mode == 1
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_missing_pairing_token_has_the_same_minimum_failure_delay() -> None:
    async def scenario() -> None:
        link = FakeLink()

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        session = DeviceSession(link, on_ready, on_event)
        session.AUTH_MISSING_TOKEN_DELAY_SECONDS = 0.01
        task = asyncio.create_task(session.run())
        started = asyncio.get_running_loop().time()
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x76543210,
                    sequence=1,
                    payload=encode_tlv(
                        [
                            (
                                TlvTag.DEVICE_ID,
                                bytes.fromhex(
                                    "00112233445566778899aabbccddeeff"
                                ),
                            ),
                            (TlvTag.BOOT_ID, struct.pack("<Q", 9)),
                            (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 1)),
                            (TlvTag.CAPABILITIES, struct.pack("<Q", 0x1FF)),
                            (TlvTag.TRANSPORT, b"\x02"),
                            (TlvTag.AUTH_MODE, b"\x01"),
                            (TlvTag.AUTH_CHALLENGE, bytes.fromhex("44" * 32)),
                            (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
                        ]
                    ),
                )
            )
        )
        with pytest.raises(ProtocolError, match="requires a pairing token"):
            await task
        assert asyncio.get_running_loop().time() - started >= 0.008

    asyncio.run(scenario())


def test_unanswered_clock_probe_closes_stale_live_session() -> None:
    async def scenario() -> None:
        link = FakeLink()

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        session = DeviceSession(
            link,
            on_ready,
            on_event,
            clock_sync_interval=0.01,
            clock_sync_timeout=0.01,
        )
        task = asyncio.create_task(session.run())
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x12345678,
                    sequence=1,
                    payload=encode_tlv(
                        [
                            (
                                TlvTag.DEVICE_ID,
                                bytes.fromhex(
                                    "00112233445566778899aabbccddeeff"
                                ),
                            ),
                            (TlvTag.BOOT_ID, struct.pack("<Q", 7)),
                            (
                                TlvTag.PROTOCOL_VERSION,
                                struct.pack("<H", 1),
                            ),
                            (TlvTag.CAPABILITIES, struct.pack("<Q", 0x0F)),
                            (TlvTag.TRANSPORT, b"\x01"),
                            (TlvTag.AUTH_MODE, b"\x00"),
                            (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
                        ]
                    ),
                )
            )
        )
        await session.wait_ready()
        for _ in range(100):
            if link.closed:
                break
            await asyncio.sleep(0.002)
        assert link.closed is True
        probe_types = [
            decode_frame(wire[:-1]).type
            for wire in link.writes
            if decode_frame(wire[:-1]).channel == Channel.CONTROL
        ]
        assert probe_types.count(ControlType.TIME_SYNC_REQUEST) == 1
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_usb_serial_jtag_keeps_link_after_unanswered_clock_probes() -> None:
    async def scenario() -> None:
        link = FakeLink()

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        session = DeviceSession(
            link,
            on_ready,
            on_event,
            clock_sync_interval=0.01,
            clock_sync_timeout=0.01,
        )
        task = asyncio.create_task(session.run())
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=0x12345678,
                    sequence=1,
                    payload=encode_tlv(
                        [
                            (
                                TlvTag.DEVICE_ID,
                                bytes.fromhex(
                                    "00112233445566778899aabbccddeeff"
                                ),
                            ),
                            (TlvTag.BOOT_ID, struct.pack("<Q", 7)),
                            (
                                TlvTag.PROTOCOL_VERSION,
                                struct.pack("<H", 1),
                            ),
                            (TlvTag.CAPABILITIES, struct.pack("<Q", 0x0F)),
                            (TlvTag.TRANSPORT, b"\x03"),
                            (TlvTag.AUTH_MODE, b"\x00"),
                            (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
                        ]
                    ),
                )
            )
        )
        await session.wait_ready()
        for _ in range(100):
            probe_types = [
                decode_frame(wire[:-1]).type
                for wire in link.writes
                if decode_frame(wire[:-1]).channel == Channel.CONTROL
            ]
            if probe_types.count(ControlType.TIME_SYNC_REQUEST) >= 2:
                break
            await asyncio.sleep(0.002)
        assert probe_types.count(ControlType.TIME_SYNC_REQUEST) >= 2
        assert link.closed is False
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_rpc_jobs_screenshot_media_ota_and_restart() -> None:
    async def scenario() -> None:
        link = FakeLink()
        media_events: list[dict[str, object]] = []

        async def on_ready(session: DeviceSession) -> None:
            pass

        async def on_event(event: dict[str, object]) -> None:
            pass

        async def on_media(event: dict[str, object]) -> None:
            media_events.append(event)

        session = DeviceSession(
            link, on_ready, on_event, on_media=on_media
        )
        task = asyncio.create_task(session.run())
        session_id = 0x10203040
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO,
                    session_id=session_id,
                    sequence=1,
                    payload=encode_tlv(
                        [
                            (
                                TlvTag.DEVICE_ID,
                                bytes.fromhex(
                                    "00112233445566778899aabbccddeeff"
                                ),
                            ),
                            (TlvTag.BOOT_ID, struct.pack("<Q", 12)),
                            (
                                TlvTag.PROTOCOL_VERSION,
                                struct.pack("<H", 1),
                            ),
                            (TlvTag.CAPABILITIES, struct.pack("<Q", 0x1FFF)),
                            (TlvTag.TRANSPORT, b"\x01"),
                            (TlvTag.AUTH_MODE, b"\x00"),
                            (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
                        ]
                    ),
                )
            )
        )
        await session.wait_ready()
        assert session._clock_task is not None
        session._clock_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await session._clock_task
        session._clock_task = None

        control_sequence = 1
        rpc_task = asyncio.create_task(
            session.rpc(2, 3, b"request", deadline_ms=250)
        )
        rpc_index, rpc_request = await wait_for_request(
            link, Channel.CONTROL, ControlType.REQUEST
        )
        assert struct.unpack_from("<HHIHH", rpc_request.payload) == (
            2,
            3,
            250,
            7,
            0,
        )
        control_sequence += 1
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.RESPONSE,
                    flags=1,
                    session_id=session_id,
                    request_id=rpc_request.request_id,
                    sequence=control_sequence,
                    payload=struct.pack("<HHiHH", 2, 3, 0, 5, 0)
                    + b"reply",
                )
            )
        )
        assert await rpc_task == b"reply"

        job_task = asyncio.create_task(session.job(77))
        _, job_request = await wait_for_request(
            link, Channel.CONTROL, ControlType.JOB_QUERY, rpc_index + 1
        )
        control_sequence += 1
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.JOB_STATUS,
                    flags=1,
                    session_id=session_id,
                    request_id=job_request.request_id,
                    sequence=control_sequence,
                    payload=struct.pack("<IHBBHHi", 77, 4, 1, 0, 500, 0, 0),
                )
            )
        )
        assert (await job_task)["progress_permille"] == 500

        screenshot_task = asyncio.create_task(
            session.screenshot({"width": 2, "height": 1, "format": 1})
        )
        screen_index, open_request = await wait_for_request(
            link, Channel.SCREEN, MediaType.OPEN
        )
        description = struct.pack("<HHHHIHH", 0, 0, 2, 1, 4, 1, 0)
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.SCREEN,
                    type=MediaType.OPENED,
                    flags=1 | 8,
                    session_id=session_id,
                    request_id=open_request.request_id,
                    stream_id=55,
                    sequence=1,
                    payload=description + struct.pack("<I", 4),
                )
            )
        )
        read_index, read_request = await wait_for_request(
            link, Channel.SCREEN, MediaType.READ, screen_index + 1
        )
        assert struct.unpack("<IHH", read_request.payload) == (0, 4, 0)
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.SCREEN,
                    type=MediaType.DATA,
                    flags=1 | 16,
                    session_id=session_id,
                    request_id=read_request.request_id,
                    stream_id=55,
                    sequence=2,
                    payload=struct.pack("<II", 0, 4) + b"shot",
                )
            )
        )
        _, close_request = await wait_for_request(
            link, Channel.SCREEN, MediaType.CLOSE, read_index + 1
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.SCREEN,
                    type=MediaType.CLOSE,
                    flags=1 | 16,
                    session_id=session_id,
                    request_id=close_request.request_id,
                    stream_id=55,
                    sequence=3,
                )
            )
        )
        actual, shot = await screenshot_task
        assert actual["width"] == 2
        assert shot == b"shot"

        mirror_task = asyncio.create_task(
            session.mirror_start(
                Channel.IMAGE, {"width": 1, "height": 1, "format": 3}
            )
        )
        mirror_index, mirror_request = await wait_for_request(
            link, Channel.IMAGE, MediaType.MIRROR_START
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.IMAGE,
                    type=MediaType.MIRROR_STATE,
                    flags=1 | 8,
                    session_id=session_id,
                    request_id=mirror_request.request_id,
                    stream_id=66,
                    sequence=1,
                    payload=mirror_request.payload,
                )
            )
        )
        assert (await mirror_task)["stream_id"] == 66
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.IMAGE,
                    type=MediaType.DATA,
                    session_id=session_id,
                    stream_id=66,
                    sequence=2,
                    payload=(
                        struct.pack("<QIIHH", 123, 9, 2, 0, 3)
                        + struct.pack("<HHHHIHH", 0, 0, 1, 1, 3, 3, 0)
                        + b"img"
                    ),
                )
            )
        )
        for _ in range(20):
            if media_events:
                break
            await asyncio.sleep(0)
        assert media_events[0]["data"] == b"img"

        image = b"abc"
        ota_progress = []

        async def record_ota_progress(item):
            ota_progress.append(item)

        ota_task = asyncio.create_task(
            session.ota_update(
                image,
                expected_sha256=hashlib.sha256(image).digest(),
                project_name="demo",
                version="1.0",
                progress_callback=record_ota_progress,
            )
        )
        ota_index, begin_request = await wait_for_request(
            link, Channel.OTA, OtaType.BEGIN
        )
        begin_response = struct.pack("<IIHB", 88, 3, 2, 4) + b"ota0"
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.OTA,
                    type=OtaType.BEGIN_RESPONSE,
                    flags=1 | 8,
                    session_id=session_id,
                    request_id=begin_request.request_id,
                    stream_id=88,
                    sequence=1,
                    payload=begin_response,
                )
            )
        )
        data_index, data_request = await wait_for_request(
            link, Channel.OTA, OtaType.DATA, ota_index + 1
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.OTA,
                    type=OtaType.DATA_RESPONSE,
                    flags=1,
                    session_id=session_id,
                    request_id=data_request.request_id,
                    stream_id=88,
                    sequence=2,
                    payload=struct.pack("<IHH", 2, 600, 0),
                )
            )
        )
        data2_index, data2_request = await wait_for_request(
            link, Channel.OTA, OtaType.DATA, data_index + 1
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.OTA,
                    type=OtaType.DATA_RESPONSE,
                    flags=1,
                    session_id=session_id,
                    request_id=data2_request.request_id,
                    stream_id=88,
                    sequence=3,
                    payload=struct.pack("<IHH", 3, 900, 0),
                )
            )
        )
        _, end_request = await wait_for_request(
            link, Channel.OTA, OtaType.END, data2_index + 1
        )
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.OTA,
                    type=OtaType.END_RESPONSE,
                    flags=1 | 16,
                    session_id=session_id,
                    request_id=end_request.request_id,
                    stream_id=88,
                    sequence=4,
                    payload=struct.pack("<Ii", 88, 0),
                )
            )
        )
        assert (await ota_task)["partition"] == "ota0"
        assert [item["progress_permille"] for item in ota_progress] == [
            0,
            600,
            900,
            950,
        ]

        restart_task = asyncio.create_task(session.restart(300))
        _, restart_request = await wait_for_request(
            link, Channel.CONTROL, ControlType.RESTART, mirror_index + 1
        )
        control_sequence += 1
        await link.incoming.put(
            encode_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.RESTART,
                    flags=1,
                    session_id=session_id,
                    request_id=restart_request.request_id,
                    sequence=control_sequence,
                    payload=struct.pack("<I", 300),
                )
            )
        )
        assert await restart_task == 300
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
