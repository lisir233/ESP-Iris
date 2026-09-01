from __future__ import annotations

import asyncio
import os
import struct

import pytest

from iris_gateway.protocol import Channel, ControlType, Frame, Transport, encode_frame

from .contracts import (
    BOUNDARY_V1,
    EXERCISE_BOUNDARY_METHOD,
    JOB_ID_V1,
    JOB_REQUEST_V1,
    LIFECYCLE_CYCLE_METHOD,
    LOG_BURST_METHOD,
    LOG_BURST_V1,
    STATE_METHOD,
    TEST_SERVICE_ID,
    FixtureState,
)
from .gateway import GatewayProcess
from .helpers import run
from .raw import RawIrisSession

pytestmark = [
    pytest.mark.iris_e2e,
    pytest.mark.iris_stage(2),
    pytest.mark.firmware_profile("services_usb"),
]


async def _open_usb(iris_board) -> RawIrisSession:
    endpoint = iris_board.discover_application_port()
    session = RawIrisSession(endpoint)
    await session.open()
    return session


def test_usb_handshake_status_ping_and_invalid_frames(
    iris_board, firmware_profile
) -> None:
    assert firmware_profile == "services_usb"

    async def scenario() -> None:
        raw = await _open_usb(iris_board)
        try:
            assert raw.session is not None
            info = raw.session.info
            assert info is not None
            assert info.transport == Transport.USB
            assert len(info.device_id) == 32
            assert info.boot_id != 0 and info.session_id != 0
            assert info.max_payload >= 1024
            for _ in range(100):
                if raw.session.clock_uncertainty_us is not None:
                    break
                await asyncio.sleep(0.02)
            assert raw.session.clock_uncertainty_us is not None

            pong = await raw.request(
                Channel.CONTROL, ControlType.PING, b"iris-ping"
            )
            assert pong.type == ControlType.PONG
            assert pong.payload == b"iris-ping"

            status_before = await raw.session.status()
            assert status_before["lifecycle_state"] == 2
            assert status_before["invalid_frames"] == 0

            with pytest.raises(RuntimeError, match="device error"):
                await raw.request(Channel.CONTROL, 0x6E)

            duplicate_ack = Frame(
                channel=Channel.CONTROL,
                type=ControlType.HELLO_ACK,
                session_id=info.session_id,
                sequence=0x100,
            )
            await raw.send_frame(duplicate_ack)
            assert (await raw.session.status())["session_id"] == info.session_id

            stale = Frame(
                channel=Channel.CONTROL,
                type=ControlType.PING,
                session_id=(info.session_id - 1) & 0xFFFFFFFF,
                sequence=0x101,
                payload=b"old-session",
            )
            await raw.send_frame(stale)
            corrupt = bytearray(
                encode_frame(
                    Frame(
                        channel=Channel.CONTROL,
                        type=ControlType.PING,
                        session_id=info.session_id,
                        sequence=0x102,
                        payload=b"bad-crc",
                    )
                )
            )
            corrupt[-3] ^= 0x55
            await raw.send_wire(bytes(corrupt))
            await asyncio.sleep(0.1)
            status_after = await raw.session.status()
            assert status_after["invalid_frames"] >= 2
        finally:
            await raw.close()

    run(scenario())


def test_rpc_jobs_log_overflow_and_resource_boundaries(
    iris_board, firmware_profile
) -> None:
    assert firmware_profile == "services_usb"

    async def wait_job(raw: RawIrisSession, job_id: int) -> dict[str, object]:
        assert raw.session is not None
        for _ in range(200):
            result = await raw.session.job(job_id)
            if result["job_state"] not in {"queued", "running"}:
                return result
            await asyncio.sleep(0.02)
        raise TimeoutError(f"job {job_id} did not finish")

    async def scenario() -> None:
        raw = await _open_usb(iris_board)
        try:
            assert raw.session is not None
            maximum = os.urandom(1024)
            assert await raw.session.rpc(1, 1, maximum) == maximum
            with pytest.raises(RuntimeError, match="RPC failed"):
                await raw.session.rpc(1, 7, struct.pack("<i", 0x102))
            with pytest.raises(RuntimeError, match="device error"):
                await raw.session.rpc(0x2222, 1)
            with pytest.raises(RuntimeError, match="RPC failed"):
                await raw.session.rpc(
                    1,
                    3,
                    struct.pack("<H", 100) + b"late",
                    deadline_ms=10,
                )

            success_id = JOB_ID_V1.unpack(
                await raw.session.rpc(1, 2, JOB_REQUEST_V1.pack(0, 0, 100))
            )[0]
            assert (await wait_job(raw, success_id))["job_state"] == "succeeded"
            failure_id = JOB_ID_V1.unpack(
                await raw.session.rpc(1, 2, JOB_REQUEST_V1.pack(1, 0, 100))
            )[0]
            assert (await wait_job(raw, failure_id))["job_state"] == "failed"
            cancel_id = JOB_ID_V1.unpack(
                await raw.session.rpc(1, 2, JOB_REQUEST_V1.pack(0, 0, 10000))
            )[0]
            await raw.session.job(cancel_id, cancel=True)
            assert (await wait_job(raw, cancel_id))["job_state"] == "cancelled"

            errors = BOUNDARY_V1.unpack(
                await raw.session.rpc(TEST_SERVICE_ID, EXERCISE_BOUNDARY_METHOD)
            )
            assert all(error != 0 for error in errors)

            await raw.session.rpc(
                TEST_SERVICE_ID,
                LOG_BURST_METHOD,
                LOG_BURST_V1.pack(32, 32, 256, 0),
                timeout=10,
            )
            state = FixtureState.decode(
                await raw.session.rpc(TEST_SERVICE_ID, STATE_METHOD)
            )
            assert state.stdout_records == 32
            assert state.stderr_records == 32
            assert state.log_bytes == 64 * 256
            assert state.log_dropped_bytes > 0
            assert await raw.session.rpc(1, 1, b"after-overflow") == b"after-overflow"
        finally:
            await raw.close()

    run(scenario())


def test_lifecycle_reconnect_preserves_identity_and_releases_resources(
    iris_board, firmware_profile
) -> None:
    assert firmware_profile == "services_usb"

    async def scenario() -> None:
        first = await _open_usb(iris_board)
        assert first.session is not None and first.session.info is not None
        device_id = first.session.info.device_id
        boot_id = first.session.info.boot_id
        before = await first.session.status()
        await first.session.rpc(TEST_SERVICE_ID, LIFECYCLE_CYCLE_METHOD)
        await asyncio.sleep(1)
        await first.close()

        second: RawIrisSession | None = None
        for _ in range(50):
            try:
                second = await _open_usb(iris_board)
                break
            except (ConnectionError, OSError, TimeoutError):
                await asyncio.sleep(0.1)
        assert second is not None and second.session is not None
        try:
            assert second.session.info is not None
            assert second.session.info.device_id == device_id
            assert second.session.info.boot_id == boot_id
            state = FixtureState.decode(
                await second.session.rpc(TEST_SERVICE_ID, STATE_METHOD)
            )
            after = await second.session.status()
            assert state.start_count == 2 and state.stop_count == 1
            assert state.register_count == 2 and state.unregister_count == 1
            assert state.last_error == 0
            assert after["task_stack_free_min_bytes"] >= 512
            assert (
                after["internal_heap_used_bytes"]
                <= before["internal_heap_used_bytes"] + 256
            )
        finally:
            await second.close()

    run(scenario())


def test_real_gateway_usb_smoke(iris_board, iris_artifacts, firmware_profile) -> None:
    assert firmware_profile == "services_usb"
    endpoint = iris_board.discover_application_port()
    with GatewayProcess(
        iris_artifacts, endpoint_kind="usb", endpoint=endpoint, name="usb-smoke"
    ) as gateway:
        api = gateway.start()
        device = api.wait_device()
        device_id = device["device_id"]
        status, body, _ = api.request(
            "GET", f"/v1/devices/{device_id}", evidence_name="usb-status"
        )
        assert status == 200
        assert body["transport"] == Transport.USB
        status, rpc, _ = api.request(
            "POST",
            f"/v1/devices/{device_id}/rpc/raw",
            json_body={
                "service_id": 1,
                "method_id": 1,
                "payload_text": "gateway-smoke",
            },
            evidence_name="usb-rpc",
        )
        assert status == 200 and rpc["response_bytes"] == len("gateway-smoke")
