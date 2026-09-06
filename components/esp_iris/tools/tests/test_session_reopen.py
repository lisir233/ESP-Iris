from __future__ import annotations

import asyncio
import struct
from unittest.mock import AsyncMock

import pytest

from iris_gateway.protocol import (
    Capability,
    Channel,
    ControlType,
    Frame,
    ProtocolError,
    TlvTag,
    decode_frame,
    encode_tlv,
)
from iris_gateway.session import DeviceSession


@pytest.mark.parametrize("authenticated", [False, True])
def test_negotiated_reopen_waits_for_fresh_session(authenticated: bool) -> None:
    async def scenario() -> None:
        link = AsyncMock()
        link.endpoint = "fake:usj"
        session = DeviceSession(link, AsyncMock(), AsyncMock(),
                                pairing_token=b"a" * 32 if authenticated else None)
        session._complete_ready = AsyncMock()
        hello = encode_tlv([
            (TlvTag.DEVICE_ID, b"d" * 16), (TlvTag.BOOT_ID, struct.pack("<Q", 7)),
            (TlvTag.PROTOCOL_VERSION, struct.pack("<H", 1)),
            (TlvTag.CAPABILITIES, struct.pack("<Q", Capability.SESSION_REOPEN)),
            (TlvTag.AUTH_MODE, bytes([int(authenticated)])),
            (TlvTag.AUTH_CHALLENGE, b"c" * 32),
        ])
        old = Frame(channel=Channel.CONTROL, type=ControlType.HELLO,
                    session_id=20, sequence=1, payload=hello)
        await session._handle_hello(old)
        ack = decode_frame(link.write.call_args.args[0][:-1])
        assert ack.flags == 1 << 5 and ack.session_id == 20
        session._complete_ready.assert_not_called()
        await session._handle_hello(old)  # lost reply: same-session repeat is safe
        session._complete_ready.assert_not_called()
        await session._handle_frame(Frame(channel=Channel.CONTROL,
            type=ControlType.AUTH_RESULT, session_id=20, payload=b"\x01"), 0)
        session._complete_ready.assert_not_called()
        session._last_rx_sequence[0] = 99
        fresh = Frame(channel=Channel.CONTROL, type=ControlType.HELLO,
                      session_id=21, sequence=1, payload=hello)
        await session._handle_hello(fresh)
        ack = decode_frame(link.write.call_args.args[0][:-1])
        assert ack.flags == 0 and ack.session_id == 21 and ack.sequence == 1
        assert session._last_rx_sequence[0] is None
        if authenticated:
            session._complete_ready.assert_not_called()
            await session._handle_frame(Frame(channel=Channel.CONTROL,
                type=ControlType.AUTH_RESULT, session_id=21, sequence=2,
                payload=b"\x01"), 0)
        session._complete_ready.assert_awaited_once()
        await session._handle_hello(fresh)
        assert decode_frame(link.write.call_args.args[0][:-1]).flags == 0
        with pytest.raises(ProtocolError):
            await session._handle_hello(old)
    asyncio.run(scenario())
