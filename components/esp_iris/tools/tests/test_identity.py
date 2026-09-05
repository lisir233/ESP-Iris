from __future__ import annotations

import asyncio
import struct

import pytest

from iris_gateway.protocol import (
    Channel,
    ControlType,
    Frame,
    ProtocolError,
    TlvTag,
    encode_tlv,
)
from iris_gateway.session import DeviceSession


class IdentityLink:
    endpoint = "fake:identity"

    def __init__(self) -> None:
        self.writes: list[bytes] = []

    async def write(self, data: bytes) -> None:
        self.writes.append(data)


def hello(extra: list[tuple[int, bytes]]) -> Frame:
    return Frame(channel=Channel.CONTROL, type=ControlType.HELLO, session_id=7,
                 payload=encode_tlv([
                     (TlvTag.DEVICE_ID, bytes(16)),
                     (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 1)),
                     (TlvTag.PROJECT_NAME, b"recovery-analysis-app"),
                     (TlvTag.APP_VERSION, b"recovery-stable-1"),
                     *extra,
                 ]))


async def discard(value: object) -> None:
    pass


@pytest.mark.parametrize("role,expected", [(None, "unknown"), (0, "unknown"),
                                            (1, "normal"), (2, "recovery")])
def test_role_is_explicit_and_legacy_peer_remains_unknown(role, expected) -> None:
    async def scenario() -> None:
        link = IdentityLink()
        session = DeviceSession(link, discard, discard)
        fields = [] if role is None else [(TlvTag.FIRMWARE_ROLE, bytes([role]))]
        fields += [(TlvTag.PRODUCT_CONTRACT, b"mosaico-v1"),
                   (TlvTag.CHIP_TARGET, b"esp32s31"),
                   (TlvTag.BOARD_ID, b"mosaico"),
                   (TlvTag.LAYOUT_ID, b"retained-v1"),
                   (TlvTag.RECOVERY_ABI, struct.pack("<H", 1)),
                   (TlvTag.HEALTH_TIMEOUT_MS, struct.pack("<I", 90000)),
                   (0xFE, b"future optional metadata")]
        try:
            await session._handle_hello(hello(fields))
            assert session.info is not None
            info = session.info.as_dict()
            assert info["firmware_mode"] == expected
            assert info["product_contract"] == "mosaico-v1"
            assert info["chip_target"] == "esp32s31"
            assert info["board_id"] == "mosaico"
            assert info["layout_id"] == "retained-v1"
            assert info["recovery_abi"] == 1
            assert info["health_timeout_ms"] == 90000
        finally:
            if session._clock_task is not None:
                session._clock_task.cancel()
                await asyncio.gather(session._clock_task, return_exceptions=True)

    asyncio.run(scenario())


@pytest.mark.parametrize("field", [
    (TlvTag.REQUIRED_FEATURES, struct.pack("<Q", 1 << 63)),
    (TlvTag.FIRMWARE_ROLE, b"\x03"),
    (TlvTag.FIRMWARE_ROLE, b"\x01\x00"),
    (TlvTag.HEALTH_TIMEOUT_MS, struct.pack("<I", 0)),
])
def test_incompatible_metadata_rejected_before_ack(field) -> None:
    async def scenario() -> None:
        link = IdentityLink()
        session = DeviceSession(link, discard, discard)
        with pytest.raises(ProtocolError):
            await session._handle_hello(hello([field]))
        assert link.writes == []
        assert session.info is None

    asyncio.run(scenario())
