import asyncio
from typing import Any

import pytest

from iris_gateway.gateway import GatewayService


class ReenumeratingHub:
    def __init__(self) -> None:
        self.status_calls = 0
        self.entered_recovery = False
        self.ota_updates = 0
        self.queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()

    def subscribe(self, device_id: str) -> asyncio.Queue[dict[str, Any]]:
        assert device_id == "device-a"
        return self.queue

    def unsubscribe(
        self, device_id: str, queue: asyncio.Queue[dict[str, Any]]
    ) -> None:
        assert device_id == "device-a"
        assert queue is self.queue

    async def status(self, device_id: str) -> dict[str, Any]:
        assert device_id == "device-a"
        self.status_calls += 1
        if self.status_calls == 1:
            return {
                "boot_id": 10,
                "firmware_mode": "normal",
                "project_name": "iris_get_started",
            }
        if self.status_calls == 2:
            raise ConnectionError("ESP-Iris session closed")
        return {
            "boot_id": 20,
            "firmware_mode": "recovery",
            "project_name": "iris_get_started",
        }

    async def enter_recovery(self, device_id: str) -> None:
        assert device_id == "device-a"
        self.entered_recovery = True

    async def ota_update(
        self,
        device_id: str,
        image: bytes,
        *,
        expected_sha256: bytes,
        project_name: str,
        version: str,
        progress_callback,
    ) -> dict[str, Any]:
        assert device_id == "device-a"
        assert image == b"firmware"
        assert expected_sha256 == bytes.fromhex("00" * 32)
        assert project_name == "iris_get_started"
        assert version == "1.0.2"
        self.ota_updates += 1
        await progress_callback(
            {
                "stage": "validated",
                "progress_permille": 1000,
                "bytes_received": len(image),
                "bytes_total": len(image),
                "partition": "ota_0",
            }
        )
        return {
            "healthy": True,
            "bytes": len(image),
            "partition": "ota_0",
        }


class RecordingOperations:
    def __init__(self) -> None:
        self.progress_updates: list[dict[str, Any]] = []

    async def progress(self, operation_id: str, **fields: Any) -> None:
        assert operation_id == "ota-op"
        self.progress_updates.append(fields)


class ProjectPolicyHub:
    def __init__(self, required: bool | None) -> None:
        self.required = required
        self.ota_updates = 0
        self.queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()

    def subscribe(self, device_id: str) -> asyncio.Queue[dict[str, Any]]:
        assert device_id == "device-a"
        return self.queue

    def unsubscribe(
        self, device_id: str, queue: asyncio.Queue[dict[str, Any]]
    ) -> None:
        assert device_id == "device-a"
        assert queue is self.queue

    async def status(self, device_id: str) -> dict[str, Any]:
        assert device_id == "device-a"
        result: dict[str, Any] = {
            "boot_id": 10,
            "firmware_mode": "normal",
            "project_name": "old-project",
        }
        if self.required is not None:
            result["ota_project_name_match_required"] = self.required
        return result

    async def ota_update(self, device_id: str, image: bytes, **kwargs) -> dict[str, Any]:
        del kwargs
        assert device_id == "device-a"
        assert image == b"firmware"
        self.ota_updates += 1
        return {"healthy": True, "bytes": len(image), "partition": "ota_0"}


class RestartRaceHub(ProjectPolicyHub):
    def __init__(self) -> None:
        super().__init__(required=False)
        self.status_calls = 0

    async def status(self, device_id: str) -> dict[str, Any]:
        self.status_calls += 1
        if self.status_calls == 1:
            return {
                "boot_id": 10,
                "firmware_mode": "recovery",
                "project_name": "esp_iris_ota",
            }
        return {
            "boot_id": 20,
            "firmware_mode": "normal",
            "project_name": "esp_iris_ota",
            "app_version": "1.0.2",
        }


    async def ota_update(self, device_id: str, image: bytes, **kwargs) -> dict[str, Any]:
        del kwargs
        assert device_id == "device-a"
        assert image == b"firmware"
        self.queue.put_nowait(
            {"kind": "connection", "connection_state": "rebooted", "boot_id": 20}
        )
        self.queue.put_nowait(
            {"kind": "event", "event_name": "healthy", "boot_id": 20}
        )
        return {
            "job_id": 1,
            "healthy": False,
            "completion_evidence": "end_response",
        }

    async def restart(self, device_id: str, delay_ms: int) -> int:
        del delay_ms
        assert device_id == "device-a"
        raise ConnectionError("ESP-Iris session closed")


class RecoveryWriteRaceHub(ReenumeratingHub):
    async def status(self, device_id: str) -> dict[str, Any]:
        assert device_id == "device-a"
        self.status_calls += 1
        if self.status_calls == 1:
            return {
                "boot_id": 10,
                "firmware_mode": "normal",
                "project_name": "iris_get_started",
            }
        if self.status_calls == 2:
            raise PermissionError("Windows removed the COM endpoint")
        return {
            "boot_id": 20,
            "firmware_mode": "recovery",
            "project_name": "iris_get_started",
        }

    async def enter_recovery(self, device_id: str) -> None:
        assert device_id == "device-a"
        self.entered_recovery = True
        raise PermissionError("Windows removed the COM endpoint")


def test_closed_loop_ota_waits_through_recovery_session_close() -> None:
    async def scenario() -> None:
        hub = ReenumeratingHub()
        operations = RecordingOperations()
        service = GatewayService.__new__(GatewayService)
        service.hub = hub
        service.operations = operations

        result = await service.closed_loop_ota(
            "device-a",
            b"firmware",
            {
                "sha256": "00" * 32,
                "project_name": "iris_get_started",
                "version": "1.0.2",
            },
            "ota-op",
            execution_mode="recovery",
        )

        assert hub.entered_recovery is True
        assert hub.status_calls == 3
        assert hub.ota_updates == 1
        assert result["recovery_boot_id"] == 20
        assert result["healthy"] is True
        assert [update["stage"] for update in operations.progress_updates[:2]] == [
            "entering_recovery",
            "waiting_recovery",
        ]

    asyncio.run(scenario())


def test_closed_loop_ota_allows_project_change_by_default() -> None:
    async def scenario() -> None:
        hub = ProjectPolicyHub(required=None)
        service = GatewayService.__new__(GatewayService)
        service.hub = hub
        service.operations = RecordingOperations()

        result = await service.closed_loop_ota(
            "device-a",
            b"firmware",
            {
                "sha256": "00" * 32,
                "project_name": "new-project",
                "version": "1.0.2",
            },
            "ota-op",
            execution_mode="application",
        )

        assert hub.ota_updates == 1
        assert result["healthy"] is True

    asyncio.run(scenario())


def test_closed_loop_ota_reconciles_recovery_rpc_write_race() -> None:
    async def scenario() -> None:
        hub = RecoveryWriteRaceHub()
        service = GatewayService.__new__(GatewayService)
        service.hub = hub
        service.operations = RecordingOperations()

        result = await service.closed_loop_ota(
            "device-a",
            b"firmware",
            {
                "sha256": "00" * 32,
                "project_name": "iris_get_started",
                "version": "1.0.2",
            },
            "ota-op",
            execution_mode="recovery",
        )

        assert hub.entered_recovery is True
        assert result["recovery_boot_id"] == 20
        assert result["healthy"] is True

    asyncio.run(scenario())


def test_closed_loop_ota_reconciles_restart_race_after_end_response() -> None:
    async def scenario() -> None:
        hub = RestartRaceHub()
        service = GatewayService.__new__(GatewayService)
        service.hub = hub
        service.operations = RecordingOperations()

        result = await service.closed_loop_ota(
            "device-a",
            b"firmware",
            {
                "sha256": "00" * 32,
                "project_name": "esp_iris_ota",
                "version": "1.0.2",
            },
            "ota-op",
            execution_mode="recovery",
        )

        assert result["boot_id"] == 20
        assert result["healthy"] is True
        assert result["planned_restart_ms"] is None

    asyncio.run(scenario())


def test_closed_loop_ota_rejects_project_change_when_required() -> None:
    async def scenario() -> None:
        hub = ProjectPolicyHub(required=True)
        service = GatewayService.__new__(GatewayService)
        service.hub = hub
        service.operations = RecordingOperations()

        with pytest.raises(ValueError, match="does not match device project"):
            await service.closed_loop_ota(
                "device-a",
                b"firmware",
                {
                    "sha256": "00" * 32,
                    "project_name": "new-project",
                    "version": "1.0.2",
                },
                "ota-op",
                execution_mode="application",
            )

        assert hub.ota_updates == 0

    asyncio.run(scenario())
