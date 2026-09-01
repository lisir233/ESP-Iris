from __future__ import annotations

import asyncio
import hashlib
import struct
import time

import pytest

from iris_gateway.protocol import Channel, CrashType

from .gateway import GatewayProcess
from .helpers import application_artifacts
from .raw import RawIrisSession

pytestmark = [
    pytest.mark.iris_e2e,
    pytest.mark.iris_stage(7),
    pytest.mark.firmware_profile("crash_recovery"),
]


def _wait_status(api, device_id: str, predicate, timeout: float = 45) -> dict:
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        status, value, _ = api.request("GET", f"/v1/devices/{device_id}")
        if status == 200:
            last = value
            if predicate(value):
                return value
        time.sleep(0.25)
    raise TimeoutError(f"device status did not converge; last={last}")


def _wait_crash_report(api, device_id: str, predicate, timeout: float = 45) -> dict:
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        status, value, _ = api.request(
            "GET", f"/v1/devices/{device_id}/crashes"
        )
        if status == 200 and value.get("reports"):
            last = value["reports"][0]
            if predicate(last):
                return last
        time.sleep(0.25)
    raise TimeoutError(f"crash report did not converge; last={last}")


def _rpc(api, device_id: str, method: int) -> None:
    status, value, _ = api.request(
        "POST",
        f"/v1/devices/{device_id}/rpc/raw",
        json_body={"service_id": 0x1400, "method_id": method, "payload_hex": ""},
    )
    assert status == 200 and value["response_bytes"] == 0


def test_real_crash_returns_to_factory_preserves_coredump_retry_and_resume(
    iris_board, iris_artifacts, iris_cli, firmware_profile
) -> None:
    assert firmware_profile == "crash_recovery"
    iris_board.flash("crash_recovery")
    candidate = iris_board.build("crash_application")
    binary, elf, map_file = application_artifacts(candidate)
    candidate_elf_sha = hashlib.sha256(elf.read_bytes()).hexdigest()

    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="discover_usb",
        endpoint="",
        name="crash-recovery",
    ) as gateway:
        api = gateway.start()
        factory = api.wait_device()
        device_id = factory["device_id"]
        factory_boot = factory["boot_id"]
        installed = iris_cli.run(
            gateway.base_url,
            [
                "ota",
                device_id,
                str(binary),
                "--elf",
                str(elf),
                "--map",
                str(map_file),
                "--execution-mode",
                "recovery",
            ],
            log_name="crash-application-install.log",
        )
        assert installed["operation"]["status"] in {"queued", "running"}

        recovered = _wait_status(
            api,
            device_id,
            lambda value: value.get("firmware_mode") == "recovery"
            and value.get("boot_id") != factory_boot,
            timeout=60,
        )
        assert recovered["device_id"] == device_id

    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="discover_usb",
        endpoint="",
        name="crash-recovery",
    ) as gateway:
        api = gateway.start()
        recovered = _wait_status(
            api,
            device_id,
            lambda value: value.get("firmware_mode") == "recovery",
        )

        report = _wait_crash_report(
            api,
            device_id,
            lambda value: value.get("previous_boot_crash") is True
            and value.get("core_dump_valid") is True,
        )
        assert report["previous_boot_crash"] is True
        assert report["core_dump_present"] is True
        assert report["core_dump_valid"] is True
        assert report["core_dump_size"] > 0
        assert report["core_dump_elf_sha256"] == candidate_elf_sha
        assert report["candidate_elf_sha256"] == candidate_elf_sha
        assert report["decode_eligible"] is True

        status, core, headers = api.request(
            "GET",
            f"/v1/devices/{device_id}/crashes/core-dump",
            timeout=60,
        )
        assert status == 200 and len(core) == report["core_dump_size"]
        assert headers["X-ESP-Iris-SHA256"] == hashlib.sha256(core).hexdigest()
        (iris_artifacts.root / "coredump.bin").write_bytes(core)

        _rpc(api, device_id, 3)
        retried = _wait_status(
            api,
            device_id,
            lambda value: value.get("firmware_mode") == "recovery"
            and value.get("boot_id") != recovered["boot_id"],
            timeout=60,
        )
        assert retried["device_id"] == device_id

        _rpc(api, device_id, 2)
        resumed = _wait_status(
            api,
            device_id,
            lambda value: value.get("firmware_mode") == "normal"
            and value.get("boot_id") != retried["boot_id"],
        )
        resumed_report = _wait_crash_report(
            api,
            device_id,
            lambda value: value.get("boot_id") == resumed["boot_id"],
        )
        assert resumed_report["previous_boot_crash"] is False
        time.sleep(4)
        assert _wait_status(
            api, device_id, lambda value: value.get("boot_id") == resumed["boot_id"]
        )["firmware_mode"] == "normal"

        status, history, _ = api.request(
            "GET", f"/v1/events?device_id={device_id}"
        )
        assert status == 200
        names = {
            event.get("data", {}).get("event_name", event.get("event_name"))
            for event in history["events"]
        }
        assert "previous_boot_crash" in names
        assert "core_dump_available" in names
        assert "planned_restart" in names

    async def verify_chunks() -> None:
        raw = RawIrisSession(iris_board.discover_application_port())
        await raw.open()
        try:
            assert raw.session is not None
            report = await raw.session.crash_report()
            total = report["core_dump_size"]
            maximum = report["core_dump_chunk_max"]
            offset = 0
            while offset < total:
                frame = await raw.request(
                    Channel.CRASH,
                    CrashType.READ_REQUEST,
                    struct.pack("<IHH", offset, maximum, 0),
                )
                returned_offset, returned_total = struct.unpack_from(
                    "<II", frame.payload
                )
                chunk = frame.payload[8:]
                assert returned_offset == offset and returned_total == total
                assert 0 < len(chunk) <= maximum
                offset += len(chunk)
                assert bool(frame.flags & 0x10) is (offset == total)
            assert offset == total
        finally:
            await raw.close()

    asyncio.run(verify_chunks())
