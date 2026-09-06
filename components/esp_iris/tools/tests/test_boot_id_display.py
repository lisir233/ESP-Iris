from __future__ import annotations

import asyncio
import json

import pytest
from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.boot_identity import boot_id_text
from iris_gateway.demo import DemoHub
from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.session import DeviceInfo
from iris_gateway.store import GatewayStore


@pytest.mark.parametrize("value", [2**53 - 1, 2**53 + 1, 12238782771570883527, 2**64 - 1])
def test_device_info_keeps_numeric_and_exact_decimal(value):
    info = DeviceInfo(
        device_id="device", boot_id=value, session_id=7, endpoint="fake",
        transport=1, project_name="test", app_version="1", idf_version="6.1",
        firmware_sha256="00" * 32, reset_reason=1, capabilities=0,
        auth_mode=0, max_payload=4000,
    )
    wire = json.loads(json.dumps(info.as_dict()))
    assert wire["boot_id"] == value
    assert wire["boot_id_text"] == str(value)


def test_nested_boot_evidence_is_copied_and_stale_alias_replaced():
    original = {"boot_id": 2**64 - 1, "boot_id_text": "stale", "checks": [
        {"previous_boot_id": 2**53 + 1, "writer_boot_id": 2**53 + 2}], "created_ns": 2**64 - 1}
    result = boot_id_text(original)
    assert result["boot_id_text"] == str(2**64 - 1)
    assert result["checks"][0]["previous_boot_id_text"] == str(2**53 + 1)
    assert result["checks"][0]["writer_boot_id_text"] == str(2**53 + 2)
    assert "created_ns_text" not in result
    assert original["boot_id_text"] == "stale"
    assert "previous_boot_id_text" not in original["checks"][0]


def test_http_live_demo_and_old_cached_status_preserve_boot_id(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test", demo=True)
        hub = DemoHub(service.on_device_event)
        service.attach_hub(hub)
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        device_id = "demo-a1b2c3d4"
        try:
            for value in (12238782771570883527, 12238782771570883528):
                hub.get(device_id)["boot_id"] = value
                assert hub.list_devices()[0]["boot_id_text"] == str(value)
                assert (await hub.status(device_id))["boot_id_text"] == str(value)
                devices = (await (await client.get("/v1/devices")).json())["devices"]
                device = next(item for item in devices if item["device_id"] == device_id)
                assert device["boot_id"] == value
                assert device["boot_id_text"] == str(value)
                status = await (await client.get(f"/v1/devices/{device_id}")).json()
                assert status["boot_id_text"] == str(value)
            store.set_setting(f"status.{device_id}", {"device_id": device_id, "boot_id": value})
            service.mode = "observe"
            service.hub = None
            cached = await (await client.get(f"/v1/devices/{device_id}")).json()
            assert cached["boot_id"] == value
            assert cached["boot_id_text"] == str(value)
            # Legacy cached list metadata has no string alias either.
            store.db.execute("UPDATE devices SET cached_json=? WHERE device_id=?",
                             (json.dumps({"device_id": device_id, "boot_id": value}), device_id))
            store.db.commit()
            assert service.list_devices()[0]["boot_id_text"] == str(value)
        finally:
            await client.close()
            await hub.close()
            store.close()
    asyncio.run(scenario())


def test_persisted_operation_and_audit_evidence_get_exact_aliases(tmp_path):
    store = GatewayStore(tmp_path)
    try:
        operation = {"operation_id": "op", "device_id": "d", "actor_type": "developer",
                         "actor_name": "test", "action": "restart", "params": {"boot_id": 7},
                         "status": "running", "created_ns": 1}
        store.create_operation(operation)
        evidence = {"verification": {"boot_id": 12238782771570883527, "previous_boot_id": 2**53 + 1}}
        result = store.update_operation("op", result_json=evidence)
        assert result["result"]["verification"]["boot_id_text"] == "12238782771570883527"
        assert result["params"] == {"boot_id": 7}
        store.add_audit(actor_type="developer", actor_name="test", action="done", details=evidence)
        assert store.audits()[0]["details"]["verification"]["previous_boot_id_text"] == str(2**53 + 1)
    finally:
        store.close()
