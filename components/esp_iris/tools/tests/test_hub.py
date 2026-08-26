import asyncio
import struct

import pytest

from iris_gateway.hub import IrisHub, _firmware_mode_from_identity
from iris_gateway.protocol import (
    Channel,
    ControlType,
    Frame,
    TlvTag,
    encode_frame,
    encode_tlv,
)


class SupervisorLink:
    endpoint = "fake:supervisor"

    def __init__(self, session_id: int, boot_id: int = 7) -> None:
        self.incoming: asyncio.Queue[bytes] = asyncio.Queue()
        self.writes: list[bytes] = []
        self.closed = False
        hello = encode_tlv(
            [
                (TlvTag.DEVICE_ID, bytes.fromhex("00112233445566778899aabbccddeeff")),
                (TlvTag.BOOT_ID, struct.pack("<Q", boot_id)),
                (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 1)),
                (TlvTag.CAPABILITIES, struct.pack("<Q", 0x0F)),
                (TlvTag.TRANSPORT, b"\x01"),
                (TlvTag.AUTH_MODE, b"\x00"),
                (TlvTag.MAX_PAYLOAD, struct.pack("<I", 4000)),
            ]
        )
        self.incoming.put_nowait(
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

    async def read(self, size: int = 4096) -> bytes:
        return await self.incoming.get()

    async def write(self, data: bytes) -> None:
        self.writes.append(data)

    async def close(self) -> None:
        self.closed = True


def test_usb_firmware_mode_uses_hello_identity_instead_of_com_port_name() -> None:
    assert _firmware_mode_from_identity("esp_iris_ota", "1.0.0-recovery") == "recovery"
    assert _firmware_mode_from_identity("esp_iris_ota", "1.0.0-a") == "normal"


def test_supervisor_retries_and_classifies_same_boot_as_reconnect() -> None:
    async def scenario() -> None:
        hub = IrisHub(
            "test", reconnect_min_seconds=0.001, reconnect_max_seconds=0.005
        )
        links: list[SupervisorLink] = []
        attempts = 0

        async def opener() -> SupervisorLink:
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                raise OSError("not present")
            link = SupervisorLink(0x1000 + attempts)
            links.append(link)
            return link

        hub._add_supervisor("fake:supervisor", opener)
        device_id = "00112233445566778899aabbccddeeff"
        for _ in range(100):
            if hub.list_devices():
                break
            await asyncio.sleep(0.002)
        assert hub.list_devices()[0]["device_id"] == device_id
        assert attempts >= 2

        await links[0].incoming.put(b"")
        for _ in range(100):
            states = [
                event.get("connection_state")
                for event in hub._history[device_id]
                if event["kind"] == "connection"
            ]
            if "reconnected" in states:
                break
            await asyncio.sleep(0.002)
        assert states[:3] == ["connected", "disconnected", "reconnected"]
        assert hub.list_endpoints()[0]["state"] == "ready"
        await hub.close()
        assert all(link.closed for link in links)

    asyncio.run(scenario())


def test_input_gesture_is_one_gateway_operation_with_fixed_pointer_rpc_frames() -> None:
    class PointerSession:
        def __init__(self) -> None:
            self.requests: list[tuple[int, int, bytes]] = []

        async def rpc(
            self,
            service_id: int,
            method_id: int,
            payload: bytes,
            *,
            deadline_ms: int,
        ) -> bytes:
            assert deadline_ms == 1000
            self.requests.append((service_id, method_id, payload))
            return payload

    async def scenario() -> None:
        hub = IrisHub("test")
        session = PointerSession()
        hub._devices["device-a"] = session  # type: ignore[assignment]
        result = await hub.input_event(
            "device-a",
            {
                "begin": {"x": 0, "y": 0},
                "moves": [{"x": 5000, "y": 5000}],
                "end": {"x": 10000, "y": 10000},
            },
        )
        assert result["points"] == 3
        assert [(service, method) for service, method, _ in session.requests] == [
            (0x1001, 1),
            (0x1001, 1),
            (0x1001, 1),
        ]
        decoded = [struct.unpack("<BBhhHI", payload) for _, _, payload in session.requests]
        assert [item[0] for item in decoded] == [0, 1, 2]
        assert [(item[2], item[3]) for item in decoded] == [(0, 0), (240, 240), (479, 479)]

    asyncio.run(scenario())


class MirrorSession:
    def __init__(self) -> None:
        self.start_calls = 0
        self.stop_calls = 0
        self.active = False

    async def mirror_start(
        self, channel: int, description: dict[str, int] | None, *, fps: int
    ) -> dict[str, object]:
        assert channel == Channel.SCREEN
        assert fps == 5
        if self.active:
            raise RuntimeError("mirror already active")
        self.active = True
        self.start_calls += 1
        return {
            "stream_id": 66,
            "description": {
                "x": 0,
                "y": 0,
                "width": 2,
                "height": 2,
                "stride": 4,
                "format": 1,
            },
        }

    async def mirror_stop(self, channel: int) -> None:
        assert channel == Channel.SCREEN
        self.active = False
        self.stop_calls += 1


class FailingMirrorSession(MirrorSession):
    async def mirror_start(
        self, channel: int, description: dict[str, int] | None, *, fps: int
    ) -> dict[str, object]:
        raise RuntimeError("mirror start failed")


class FailingMirrorStopSession(MirrorSession):
    async def mirror_stop(self, channel: int) -> None:
        assert channel == Channel.SCREEN
        self.stop_calls += 1
        raise RuntimeError("mirror stop failed")


def _screen_tile(frame_id: int, y: int, data: bytes) -> dict[str, object]:
    return {
        "device_id": "device-a",
        "channel": int(Channel.SCREEN),
        "stream_id": 66,
        "frame_id": frame_id,
        "description": {
            "x": 0,
            "y": y,
            "width": 2,
            "height": 1,
            "stride": 4,
            "format": 1,
        },
        "data": data,
    }


def test_screenshot_uses_a_temporary_mirror_and_assembles_one_frame() -> None:
    async def scenario() -> None:
        hub = IrisHub("test")
        session = MirrorSession()
        hub._devices["device-a"] = session  # type: ignore[assignment]
        task = asyncio.create_task(hub.screenshot("device-a"))
        while session.start_calls == 0:
            await asyncio.sleep(0)
        await hub._on_media(_screen_tile(7, 0, b"abcd"))
        await hub._on_media(_screen_tile(7, 1, b"efgh"))
        description, data = await task
        assert data == b"abcdefgh"
        assert description["mirror_reused"] == 0
        assert session.start_calls == 1
        assert session.stop_calls == 1
        assert session.active is False

    asyncio.run(scenario())


def test_screenshot_reuses_running_mirror_without_stopping_it() -> None:
    async def scenario() -> None:
        hub = IrisHub("test")
        session = MirrorSession()
        hub._devices["device-a"] = session  # type: ignore[assignment]
        await hub.mirror_start("device-a", int(Channel.SCREEN), fps=5)
        task = asyncio.create_task(hub.screenshot("device-a"))
        await asyncio.sleep(0)
        await hub._on_media(_screen_tile(8, 1, b"old!"))
        await hub._on_media(_screen_tile(9, 0, b"abcd"))
        await hub._on_media(_screen_tile(9, 1, b"efgh"))
        description, data = await task
        assert data == b"abcdefgh"
        assert description["mirror_reused"] == 1
        assert session.start_calls == 1
        assert session.stop_calls == 0
        assert session.active is True

    asyncio.run(scenario())


def test_repeated_mirror_start_reuses_the_running_stream() -> None:
    async def scenario() -> None:
        hub = IrisHub("test")
        session = MirrorSession()
        hub._devices["device-a"] = session  # type: ignore[assignment]
        first = await hub.mirror_start("device-a", int(Channel.SCREEN), fps=5)
        second = await hub.mirror_start("device-a", int(Channel.SCREEN), fps=5)
        assert first["reused"] is False
        assert second["reused"] is True
        assert second["stream_id"] == first["stream_id"]
        assert session.start_calls == 1
        assert session.active is True

    asyncio.run(scenario())


def test_failed_mirror_stop_preserves_the_running_stream_state() -> None:
    async def scenario() -> None:
        hub = IrisHub("test")
        session = FailingMirrorStopSession()
        hub._devices["device-a"] = session  # type: ignore[assignment]
        await hub.mirror_start("device-a", int(Channel.SCREEN), fps=5)
        with pytest.raises(RuntimeError, match="mirror stop failed"):
            await hub.mirror_stop("device-a", int(Channel.SCREEN))
        reused = await hub.mirror_start("device-a", int(Channel.SCREEN), fps=5)
        assert reused["reused"] is True
        assert session.start_calls == 1

    asyncio.run(scenario())


def test_failed_temporary_mirror_stop_preserves_the_stream_state() -> None:
    async def scenario() -> None:
        hub = IrisHub("test")
        session = FailingMirrorStopSession()
        hub._devices["device-a"] = session  # type: ignore[assignment]
        task = asyncio.create_task(hub.screenshot("device-a"))
        while session.start_calls == 0:
            await asyncio.sleep(0)
        await hub._on_media(_screen_tile(10, 0, b"abcd"))
        await hub._on_media(_screen_tile(10, 1, b"efgh"))
        with pytest.raises(RuntimeError, match="mirror stop failed"):
            await task
        reused = await hub.mirror_start("device-a", int(Channel.SCREEN), fps=5)
        assert reused["reused"] is True
        assert session.start_calls == 1

    asyncio.run(scenario())


def test_screenshot_does_not_stop_a_mirror_that_failed_to_start() -> None:
    async def scenario() -> None:
        hub = IrisHub("test")
        session = FailingMirrorSession()
        hub._devices["device-a"] = session  # type: ignore[assignment]
        with pytest.raises(RuntimeError, match="mirror start failed"):
            await hub.screenshot("device-a")
        assert session.stop_calls == 0
        assert session.active is False

    asyncio.run(scenario())
