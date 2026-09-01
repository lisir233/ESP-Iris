from __future__ import annotations

import asyncio
import time

import pytest

from iris_gateway.protocol import ProtocolError, Transport

from .gateway import GatewayProcess
from .raw import RawIrisSession

pytestmark = [
    pytest.mark.iris_e2e,
    pytest.mark.iris_stage(5),
    pytest.mark.firmware_profile("coredump_tcp"),
]


def test_tcp_pairing_delay_rotation_persistence_and_single_owner(
    iris_board, iris_artifacts, iris_e2e_config, firmware_profile
) -> None:
    assert firmware_profile == "coredump_tcp"
    marker = iris_board.wait_console_marker(
        r"IRIS_TCP_PAIRING_READY ip=(?P<ip>\d+\.\d+\.\d+\.\d+) "
        r"port=(?P<port>\d+)",
        log_name="console-tcp-pairing.log",
    )
    host = marker.group("ip")
    port = int(marker.group("port"))
    token = iris_e2e_config.secrets.pairing_token
    next_token = iris_e2e_config.secrets.next_pairing_token

    async def failed(candidate: str | None) -> float:
        deadline = asyncio.get_running_loop().time() + 5
        while True:
            raw = RawIrisSession(
                str(port), tcp_host=host, pairing_token=candidate
            )
            started = time.monotonic()
            elapsed = 0.0
            try:
                with pytest.raises(
                    (ProtocolError, ConnectionError, TimeoutError, OSError)
                ):
                    await raw.open()
            finally:
                elapsed = time.monotonic() - started
                await raw.close()
                # A fast rejection can be the single-owner guard while the
                # previous peer FIN is still being observed. It is not an auth
                # result, so retry within a fixed boundary without counting
                # cleanup time as authentication delay.
                await asyncio.sleep(0.05)
            if elapsed >= 0.4 or asyncio.get_running_loop().time() >= deadline:
                return elapsed

    async def open_with_retry(candidate: str, timeout: float = 20) -> RawIrisSession:
        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            candidate_session = RawIrisSession(
                str(port), tcp_host=host, pairing_token=candidate
            )
            try:
                await candidate_session.open()
                return candidate_session
            except (ProtocolError, ConnectionError, TimeoutError, OSError):
                await candidate_session.close()
                if asyncio.get_running_loop().time() >= deadline:
                    raise
                await asyncio.sleep(0.25)

    async def scenario() -> None:
        assert await failed(None) >= 0.4
        assert await failed("00" * 32) >= 0.4

        raw = RawIrisSession(str(port), tcp_host=host, pairing_token=token)
        await raw.open()
        try:
            assert raw.session is not None and raw.session.info is not None
            assert raw.session.info.transport == Transport.TCP
            device_id = raw.session.info.device_id
            boot_id = raw.session.info.boot_id

            second = RawIrisSession(
                str(port), tcp_host=host, pairing_token=token
            )
            with pytest.raises(
                (ProtocolError, ConnectionError, TimeoutError, OSError)
            ):
                await asyncio.wait_for(second.open(), timeout=1)
            await second.close()

            await raw.session.rpc(1, 3)
        finally:
            await raw.close()

        assert await failed(token) >= 0.4
        rotated = await open_with_retry(next_token)
        try:
            assert rotated.session is not None and rotated.session.info is not None
            assert rotated.session.info.device_id == device_id
            await rotated.session.restart(100)
        finally:
            await rotated.close()
        await asyncio.sleep(1)

        persisted = await open_with_retry(next_token)
        try:
            assert persisted.session is not None and persisted.session.info is not None
            assert persisted.session.info.device_id == device_id
            assert persisted.session.info.boot_id != boot_id
        finally:
            await persisted.close()

    asyncio.run(scenario())

    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="tcp",
        endpoint=f"{host}:{port}",
        pairing_token=next_token,
        name="tcp-paired",
    ) as gateway:
        api = gateway.start()
        device = api.wait_device()
        assert device["transport"] == Transport.TCP
        with GatewayProcess(
            iris_artifacts,
            endpoint_kind="tcp",
            endpoint=f"{host}:{port}",
            pairing_token=next_token,
            name="tcp-second-owner",
        ) as second_gateway:
            with pytest.raises(
                RuntimeError,
                match="endpoint is owned by another ESP-Iris instance",
            ):
                second_gateway.start()
            status, still_owned, _ = api.request(
                "GET", f"/v1/devices/{device['device_id']}"
            )
            assert status == 200
            assert still_owned["device_id"] == device["device_id"]
