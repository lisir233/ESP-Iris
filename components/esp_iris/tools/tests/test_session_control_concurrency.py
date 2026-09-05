from __future__ import annotations

import asyncio
from unittest.mock import AsyncMock

import pytest

from iris_gateway.protocol import Channel, ControlType, Frame, decode_frame
from iris_gateway.session import DeviceInfo, DeviceSession


def ready_session():
    link = AsyncMock()
    link.endpoint = "fake:control-concurrency"
    session = DeviceSession(link, AsyncMock(), AsyncMock())
    session.info = DeviceInfo(
        device_id="device", boot_id=1, session_id=7, endpoint=link.endpoint,
        transport=1, project_name="test", app_version="1", idf_version="6.1",
        firmware_sha256="00" * 32, reset_reason=1, capabilities=0,
        auth_mode=0, max_payload=4000,
    )
    session._ready.set()
    return session, link


async def wait_for_writes(link, count: int) -> list[Frame]:
    for _ in range(100):
        if link.write.await_count >= count:
            return [decode_frame(call.args[0][:-1]) for call in link.write.await_args_list]
        await asyncio.sleep(0)
    raise AssertionError(f"expected {count} writes, got {link.write.await_count}")


@pytest.mark.parametrize("type_", [ControlType.STATUS_REQUEST, ControlType.PING,
                                   ControlType.TIME_SYNC_REQUEST])
def test_readonly_control_probe_bypasses_pending_rpc(type_) -> None:
    async def scenario() -> None:
        session, link = ready_session()
        rpc = asyncio.create_task(session.rpc(0x6A02, 1, timeout=10))
        await wait_for_writes(link, 1)
        if type_ == ControlType.STATUS_REQUEST:
            probe = asyncio.create_task(session.status())
        else:
            probe = asyncio.create_task(session._request(Channel.CONTROL, type_))
        frames = await wait_for_writes(link, 2)
        assert frames[0].type == ControlType.REQUEST
        assert frames[1].type == type_
        assert frames[0].request_id != frames[1].request_id
        assert frames[1].sequence == frames[0].sequence + 1
        response_type = {
            ControlType.STATUS_REQUEST: ControlType.STATUS_RESPONSE,
            ControlType.PING: ControlType.PONG,
            ControlType.TIME_SYNC_REQUEST: ControlType.TIME_SYNC_RESPONSE,
        }[type_]
        await session._handle_frame(Frame(
            channel=Channel.CONTROL, type=response_type, session_id=7,
            sequence=1, request_id=frames[1].request_id,
        ), 0)
        result = await asyncio.wait_for(probe, 1)
        if type_ == ControlType.STATUS_REQUEST:
            assert result["device_id"] == "device"
        assert not rpc.done()
        await session.close()
        with pytest.raises(ConnectionError, match="session closed"):
            await rpc

    asyncio.run(scenario())


def test_mutating_request_stays_serial_and_close_releases_all_waiters() -> None:
    async def scenario() -> None:
        session, link = ready_session()
        first = asyncio.create_task(session.rpc(0x6A02, 1, timeout=10))
        await wait_for_writes(link, 1)
        queued = asyncio.create_task(session.rpc(0x6A02, 2, timeout=10))
        status = asyncio.create_task(session.status())
        frames = await wait_for_writes(link, 2)
        assert [frame.type for frame in frames] == [ControlType.REQUEST, ControlType.STATUS_REQUEST]
        assert len(session._pending) == 2
        await session.close()
        results = await asyncio.wait_for(asyncio.gather(
            first, queued, status, return_exceptions=True
        ), 1)
        assert all(isinstance(result, ConnectionError) for result in results)
        assert not session._pending
        assert not session._request_lock.locked()
        assert link.write.await_count == 2
        link.close.assert_awaited_once()

    asyncio.run(scenario())
