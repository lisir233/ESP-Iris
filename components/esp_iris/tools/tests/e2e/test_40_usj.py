from __future__ import annotations

import asyncio

import pytest

from iris_gateway.discovery import discover_iris_usb_devices
from iris_gateway.protocol import Channel, ControlType, Frame, Transport

from .contracts import STOP_FOR_FLASH_METHOD, TEST_SERVICE_ID
from .gateway import GatewayProcess
from .helpers import run
from .raw import RawIrisSession

pytestmark = [
    pytest.mark.iris_e2e,
    pytest.mark.iris_stage(4),
    pytest.mark.firmware_profile("services_usj"),
]


def test_usb_serial_jtag_requires_opt_in_and_reuses_physical_session(
    iris_board, iris_artifacts, firmware_profile
) -> None:
    assert firmware_profile == "services_usj"
    default_paths = {
        device.path.lower()
        for device in discover_iris_usb_devices(include_usb_serial_jtag=False)
    }
    assert iris_board.config.program_port.lower() not in default_paths

    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="usj",
        endpoint=iris_board.config.program_port,
        name="usj-explicit",
    ) as gateway:
        api = gateway.start()
        first = api.wait_device()
        assert first["transport"] == Transport.USB_SERIAL_JTAG
        device_id = first["device_id"]
        boot_id = first["boot_id"]
        session_id = first["session_id"]

    async def scenario() -> None:
        raw = RawIrisSession(
            iris_board.config.program_port, usb_serial_jtag=True
        )
        await raw.open()
        try:
            assert raw.session is not None and raw.session.info is not None
            assert raw.session.info.device_id == device_id
            assert raw.session.info.boot_id == boot_id
            assert raw.session.info.session_id == session_id
            await raw.send_frame(
                Frame(
                    channel=Channel.CONTROL,
                    type=ControlType.HELLO_ACK,
                    session_id=session_id,
                    sequence=0x500,
                )
            )
            async def missed_clock_probe(timeout: float) -> None:
                del timeout
                raise TimeoutError("deliberately unanswered USJ clock probe")

            raw.session.sync_clock = missed_clock_probe  # type: ignore[method-assign]
            raw.session._clock_sync_interval = 0.05
            await asyncio.sleep(0.2)
            assert raw.task is not None and not raw.task.done()
            status = await raw.session.status()
            assert status["boot_id"] == boot_id
            assert status["session_id"] == session_id
            await raw.session.rpc(TEST_SERVICE_ID, STOP_FOR_FLASH_METHOD)
            await asyncio.sleep(0.5)
        finally:
            await raw.close()

    run(scenario())
