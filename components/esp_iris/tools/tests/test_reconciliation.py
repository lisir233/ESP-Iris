from __future__ import annotations

import asyncio
from types import SimpleNamespace

import pytest
from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.cli import build_parser
from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.operations import OperationManager, OperationOutcomeUnknown
from iris_gateway.reconciliation import (
    health_timeout,
    reconcile_operation,
    reconciliation_records,
)
from iris_gateway.security import Actor
from iris_gateway.store import GatewayStore


def uncertain(store):
    store.create_operation({
        "operation_id": "op", "device_id": "a", "actor_type": "agent", "actor_name": "test",
        "action": "firmware.ota", "params": {"image": {"elf_sha256": "11" * 32, "project_name": "app"}},
        "status": "outcome_unknown", "created_ns": 1,
    })
    store.update_operation("op", progress_json={"writer_boot_id": 1})


@pytest.mark.parametrize("healthy,sha,boot,outcome", [
    (True, "11" * 32, 2, "observed_success"),
    (False, "11" * 32, 2, "outcome_unknown"),
    (True, "22" * 32, 2, "outcome_unknown"),
    (True, "11" * 32, 1, "outcome_unknown"),
])
def test_readonly_reconciliation_keeps_history(tmp_path, healthy, sha, boot, outcome):
    async def scenario():
        store = GatewayStore(tmp_path)
        uncertain(store)
        original = store.operation("op")
        if healthy:
            store.append_event("device", {"event_name": "healthy", "boot_id": boot}, "a")

        async def status(device_id):
            assert device_id == "a"
            return {"device_id": "a", "boot_id": boot, "firmware_sha256": sha, "project_name": "app", "firmware_mode": "normal"}

        # This hub has no mutation methods, so any accidental replay fails.
        result = await reconcile_operation(store, SimpleNamespace(status=status), "op", Actor("agent", "test"))
        assert result["outcome"] == outcome
        assert result["replayed"] is False
        assert store.operation("op") == original
        store.close()
        store = GatewayStore(tmp_path)
        assert reconciliation_records(store, "op") == [result]
        store.close()

    asyncio.run(scenario())


def test_reconciliation_http_retains_disconnection_evidence(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        uncertain(store)
        service = GatewayService(store, instance_id="test")

        async def status(device_id):
            raise ConnectionError("offline")

        service.hub = SimpleNamespace(status=status)
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            response = await client.post("/v1/operations/op/reconcile")
            assert response.status == 201
            result = await response.json()
            assert result["outcome"] == "outcome_unknown"
            assert "ConnectionError" in result["reason"]
            response = await client.get("/v1/operations/op/reconciliations")
            assert (await response.json())["reconciliations"] == [result]
        finally:
            await client.close()
            store.close()

    asyncio.run(scenario())


def test_product_health_timeout_and_cli_override():
    assert health_timeout({"health_timeout_ms": 123000}) == 123
    assert health_timeout({"health_timeout_ms": 123000}, 4) == 4
    assert health_timeout({}) == 45
    for invalid in [0, 601, float("nan"), float("inf")]:
        with pytest.raises(ValueError):
            health_timeout({}, invalid)
    args = build_parser().parse_args(["web", "--ota-health-timeout", "123"])
    assert args.ota_health_timeout == 123
    args = build_parser().parse_args(["ctl", "operation-reconcile", "op"])
    assert args.operation_id == "op"


def test_missing_postwrite_health_is_unknown_and_never_replayed(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test", ota_health_timeout=1)
        queue = asyncio.Queue()
        writes = []

        async def status(device_id):
            return {"device_id": "a", "boot_id": 1, "firmware_mode": "recovery", "chip_target": "esp32s31"}

        async def ota_update(*args, **kwargs):
            writes.append(1)
            return {"completion_evidence": "session_close"}

        service.hub = SimpleNamespace(status=status, ota_update=ota_update, list_devices=list,
                                      subscribe=lambda device: queue, unsubscribe=lambda *args: None)
        with pytest.raises(OperationOutcomeUnknown):
            await service.operations.execute(
                "a", Actor("agent", "test"), "firmware.ota", {},
                lambda: service.closed_loop_ota("a", b"image", {
                    "sha256": "11" * 32, "project_name": "app", "version": "1", "chip_id": 0x20}, "op"),
                operation_id="op")
        assert store.operation("op")["status"] == "outcome_unknown"
        assert store.operation("op")["progress"]["writer_boot_id"] == 1
        assert writes == [1]
        store.close()

    asyncio.run(scenario())


def test_shutdown_during_write_is_unknown(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        manager = OperationManager(store, lambda event: asyncio.sleep(0))
        started = asyncio.Event()

        async def write():
            started.set()
            await asyncio.Event().wait()

        await manager.submit("a", Actor("agent", "test"), "firmware.ota", {}, write, operation_id="op")
        await started.wait()
        await manager.close()
        assert store.operation("op")["status"] == "outcome_unknown"
        store.close()

    asyncio.run(scenario())


@pytest.mark.parametrize("receipt,result,outcome", [
    (True, 0, "observed_success"), (True, 1, "observed_failure"), (False, 0, "outcome_unknown")
])
@pytest.mark.parametrize("target_case", ["match", "wrong_bootloader", "wrong_app", "wrong_project", "missing_identity", "wrong_binary_binding", "reboot"])
def test_system_update_requires_matching_commit_receipt(tmp_path, receipt, result, outcome, target_case):
    import uuid
    import zipfile

    async def scenario():
        store = GatewayStore(tmp_path)
        store.create_operation({
            "operation_id": "system-op", "device_id": "a", "actor_type": "agent", "actor_name": "test",
            "action": "firmware.system_update", "params": {"bundle": {"target_layout_sha256": "22" * 32, "components": [
                {"kind": "bootloader", "sha256": "33" * 32},
                {"kind": "application", "sha256": "44" * 32}]}},
            "status": "outcome_unknown", "created_ns": 1,
        })
        store.update_operation("system-op", progress_json={"writer_boot_id": 1,
            "target_application": None if target_case == "missing_identity" else {
                "sha256": "99" * 32 if target_case == "wrong_binary_binding" else "44" * 32,
                "elf_sha256": "55" * 32, "project_name": "app"}})
        original = store.operation("system-op")
        store.append_event("device", {"event_name": "healthy", "boot_id": 2}, "a")

        status_reads = []

        async def status(device_id):
            status_reads.append(1)
            return {"device_id": "a", "boot_id": 3 if target_case == "reboot" and len(status_reads) > 1 else 2,
                    "firmware_mode": "normal", "project_name": "other" if target_case == "wrong_project" else "app",
                    "firmware_sha256": "66" * 32 if target_case == "wrong_app" else "55" * 32}

        async def inventory(device_id):
            return {"last_operation_id": uuid.uuid5(uuid.NAMESPACE_URL, "system-op").hex if receipt else "other",
                    "last_result": result, "partition_table_sha256": "22" * 32,
                    "bootloader_sha256": "66" * 32 if target_case == "wrong_bootloader" else "33" * 32}

        record = await reconcile_operation(store, SimpleNamespace(status=status, system_update_inventory=inventory),
                                           "system-op", Actor("agent", "test"))
        expected = outcome
        if (outcome == "observed_success" and target_case != "match") or target_case == "reboot":
            expected = "outcome_unknown"
        assert record["outcome"] == expected
        assert store.operation("system-op") == original
        with zipfile.ZipFile(store.export_zip()) as archive:
            assert b'system-op' in archive.read("reconciliations.jsonl")
        store.close()

    asyncio.run(scenario())
