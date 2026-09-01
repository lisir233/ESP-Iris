from __future__ import annotations

import hashlib
import struct
import time

import pytest

from iris_gateway.firmware import inspect_firmware_image
from iris_gateway.protocol import Channel, OtaType

from .gateway import GatewayProcess
from .helpers import application_artifacts, run
from .raw import RawIrisSession
from .runner import CommandError

pytestmark = [
    pytest.mark.iris_e2e,
    pytest.mark.iris_stage(6),
    pytest.mark.firmware_profile("ota_recovery"),
]


def _ota_arguments(
    device_id: str, build_dir, *, execution_mode: str, wait: bool = True
) -> list[str]:
    binary, elf, map_file = application_artifacts(build_dir)
    arguments = [
        "ota",
        device_id,
        str(binary),
        "--elf",
        str(elf),
        "--map",
        str(map_file),
        "--execution-mode",
        execution_mode,
    ]
    if wait:
        arguments.extend(["--wait", "--interval", "0.1"])
    return arguments


def _assert_succeeded(result: dict) -> dict:
    operation = result["operation"]
    assert operation["status"] == "succeeded"
    assert operation.get("error") in {None, ""}
    return operation


def _wait_status(api, device_id: str, predicate, timeout: float = 45) -> dict:
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        status, value, _ = api.request("GET", f"/v1/devices/{device_id}")
        if status == 200:
            last = value
            if predicate(value):
                return value
        time.sleep(0.2)
    raise TimeoutError(f"device status did not converge; last={last}")


def test_recovery_first_closes_recovery_a_recovery_b_loop(
    iris_board, iris_artifacts, iris_cli, firmware_profile
) -> None:
    assert firmware_profile == "ota_recovery"
    iris_board.flash("ota_recovery")
    build_a = iris_board.build("ota_a")
    build_b = iris_board.build("ota_b")
    version_b = inspect_firmware_image(application_artifacts(build_b)[0].read_bytes()).version

    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="discover_usb",
        endpoint="",
        name="ota-recovery-loop",
    ) as gateway:
        api = gateway.start()
        recovery = api.wait_device()
        device_id = recovery["device_id"]
        assert recovery["firmware_mode"] == "recovery"

        first = iris_cli.run(
            gateway.base_url,
            _ota_arguments(device_id, build_a, execution_mode="recovery"),
            log_name="ota-recovery-to-a.log",
        )
        first_operation = _assert_succeeded(first)
        assert first_operation["result"]["execution_mode"] == "recovery"

        second = iris_cli.run(
            gateway.base_url,
            _ota_arguments(device_id, build_b, execution_mode="recovery"),
            log_name="ota-a-via-recovery-to-b.log",
        )
        second_operation = _assert_succeeded(second)
        result = second_operation["result"]
        assert result["execution_mode"] == "recovery"
        assert result["previous_boot_id"] != result["recovery_boot_id"]
        assert result["boot_id"] != result["recovery_boot_id"]

        status, final, _ = api.request("GET", f"/v1/devices/{device_id}")
        assert status == 200
        assert final["device_id"] == device_id
        assert final["app_version"] == version_b
        assert final["firmware_mode"] == "normal"


@pytest.mark.parametrize(
    ("writer_profile", "execution_mode"),
    [
        ("ota_application", "application"),
        ("ota_fallback", "application"),
    ],
)
def test_direct_application_and_explicit_fallback(
    iris_board,
    iris_artifacts,
    iris_cli,
    firmware_profile,
    writer_profile,
    execution_mode,
) -> None:
    assert firmware_profile == "ota_recovery"
    iris_board.flash(writer_profile)
    target = iris_board.build("ota_b")
    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="discover_usb",
        endpoint="",
        name=f"ota-{writer_profile}",
    ) as gateway:
        api = gateway.start()
        device = api.wait_device()
        result = iris_cli.run(
            gateway.base_url,
            _ota_arguments(
                device["device_id"], target, execution_mode=execution_mode
            ),
            log_name=f"ota-{writer_profile}.log",
        )
        operation = _assert_succeeded(result)
        assert operation["result"]["execution_mode"] == execution_mode


def test_cross_project_default_and_project_name_enforcement(
    iris_board, iris_artifacts, iris_cli, firmware_profile
) -> None:
    assert firmware_profile == "ota_recovery"
    cross_project_target = iris_board.build("crash_application_stable")

    iris_board.flash("ota_application")
    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="discover_usb",
        endpoint="",
        name="ota-cross-project",
    ) as gateway:
        api = gateway.start()
        device = api.wait_device()
        allowed = iris_cli.run(
            gateway.base_url,
            _ota_arguments(
                device["device_id"],
                cross_project_target,
                execution_mode="application",
            ),
            log_name="ota-cross-project-allowed.log",
        )
        _assert_succeeded(allowed)

    iris_board.flash("ota_project_match")
    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="discover_usb",
        endpoint="",
        name="ota-project-match",
    ) as gateway:
        api = gateway.start()
        device = api.wait_device()
        assert device["project_name"] == "esp_iris_ota"
        assert device["ota_project_name_match_required"] is True
        with pytest.raises(CommandError):
            iris_cli.run(
                gateway.base_url,
                _ota_arguments(
                    device["device_id"],
                    cross_project_target,
                    execution_mode="application",
                ),
                log_name="ota-project-mismatch-rejected.log",
            )
        accepted = iris_cli.run(
            gateway.base_url,
            _ota_arguments(
                device["device_id"],
                iris_board.build("ota_a"),
                execution_mode="application",
            ),
            log_name="ota-project-match-accepted.log",
        )
        _assert_succeeded(accepted)


def test_pending_rollback_returns_to_last_good_until_explicitly_accepted(
    iris_board, iris_artifacts, iris_cli, firmware_profile
) -> None:
    assert firmware_profile == "ota_recovery"
    iris_board.flash("ota_recovery")
    rollback = iris_board.build("ota_rollback")
    expected = inspect_firmware_image(
        application_artifacts(rollback)[0].read_bytes()
    ).version
    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="discover_usb",
        endpoint="",
        name="ota-rollback",
    ) as gateway:
        api = gateway.start()
        device = api.wait_device()
        device_id = device["device_id"]
        installed = iris_cli.run(
            gateway.base_url,
            _ota_arguments(
                device_id, rollback, execution_mode="recovery", wait=False
            ),
            log_name="ota-rollback-install.log",
        )
        assert installed["operation"]["status"] in {"queued", "running"}
        _wait_status(
            api,
            device_id,
            lambda value: value.get("app_version") == expected,
        )

    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="discover_usb",
        endpoint="",
        name="ota-rollback-restart",
    ) as gateway:
        api = gateway.start()
        pending = api.wait_device()
        assert pending["device_id"] == device_id
        assert pending["app_version"] == expected
        status, rolled_back, _ = api.request(
            "POST",
            f"/v1/devices/{device_id}/restart",
            json_body={"delay_ms": 100},
            timeout=40,
        )
        assert status == 200
        assert rolled_back["restart"]["reconnected"] is True
        status, last_good, _ = api.request("GET", f"/v1/devices/{device_id}")
        assert status == 200 and last_good["firmware_mode"] == "recovery"

        reinstalled = iris_cli.run(
            gateway.base_url,
            _ota_arguments(
                device_id, rollback, execution_mode="recovery", wait=False
            ),
            log_name="ota-rollback-reinstall.log",
        )
        assert reinstalled["operation"]["status"] in {"queued", "running"}
        _wait_status(
            api,
            device_id,
            lambda value: value.get("app_version") == expected,
        )

    with GatewayProcess(
        iris_artifacts,
        endpoint_kind="discover_usb",
        endpoint="",
        name="ota-rollback-accept",
    ) as gateway:
        api = gateway.start()
        pending = api.wait_device()
        assert pending["device_id"] == device_id
        assert pending["app_version"] == expected
        status, accepted, _ = api.request(
            "POST",
            f"/v1/devices/{device_id}/rpc/raw",
            json_body={"service_id": 0x1200, "method_id": 2, "payload_hex": ""},
        )
        assert status == 200 and accepted["response_bytes"] == 0
        status, restarted, _ = api.request(
            "POST",
            f"/v1/devices/{device_id}/restart",
            json_body={"delay_ms": 100},
            timeout=40,
        )
        assert status == 200
        assert restarted["restart"]["reconnected"] is True
        status, final, _ = api.request("GET", f"/v1/devices/{device_id}")
        assert status == 200 and final["app_version"] == expected


def test_raw_ota_status_cancel_offsets_hash_and_size_leave_image_unchanged(
    iris_board, firmware_profile
) -> None:
    assert firmware_profile == "ota_recovery"
    iris_board.flash("ota_recovery")
    invalid_hash_image = application_artifacts(iris_board.build("ota_a"))[0]
    invalid_hash_metadata = inspect_firmware_image(invalid_hash_image.read_bytes())

    async def scenario() -> None:
        raw = RawIrisSession(iris_board.discover_application_port())
        info = await raw.open()
        try:
            assert raw.session is not None
            original_hash = info.firmware_sha256

            def begin_payload(total: int, digest: bytes) -> bytes:
                return struct.pack("<I", total) + digest + b"\0\0\0\0"

            begin = await raw.request(
                Channel.OTA,
                OtaType.BEGIN,
                begin_payload(4, hashlib.sha256(b"good").digest()),
            )
            assert begin.type == OtaType.BEGIN_RESPONSE
            status = await raw.session.ota_status()
            assert status["active"] is True and status["bytes_received"] == 0
            with pytest.raises(RuntimeError, match="device error"):
                await raw.request(
                    Channel.OTA,
                    OtaType.DATA,
                    struct.pack("<I", 1) + b"bad",
                    stream_id=begin.stream_id,
                )
            cancelled = await raw.request(
                Channel.OTA, OtaType.CANCEL, stream_id=begin.stream_id
            )
            assert cancelled.type == OtaType.STATUS
            assert (await raw.session.ota_status())["active"] is False

            with pytest.raises(RuntimeError, match="OTA failed with device error"):
                await raw.session.ota_update(
                    invalid_hash_image.read_bytes(),
                    expected_sha256=b"\0" * 32,
                    project_name=invalid_hash_metadata.project_name,
                    version=invalid_hash_metadata.version,
                )

            with pytest.raises(RuntimeError, match="device error"):
                await raw.request(
                    Channel.OTA,
                    OtaType.BEGIN,
                    begin_payload(7 * 1024 * 1024, b"\0" * 32),
                )
            assert (await raw.session.status())["firmware_sha256"] == original_hash
        finally:
            await raw.close()

    run(scenario())
