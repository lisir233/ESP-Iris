from __future__ import annotations

import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.demo import DemoHub
from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.store import GatewayStore


@pytest.mark.parametrize("first_error", [ConnectionError, OSError, KeyError, asyncio.TimeoutError])
def test_maintenance_retries_only_live_observation_during_reenumeration(tmp_path, first_error):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test")
        lease = {
            "lease_id": "lease", "device_id": "a", "state": "detached",
            "endpoint": {"endpoint": "usb:a"}, "previous_boot_id": "old",
            "expected_version": "recovery-1", "evidence": {},
        }
        status = {"device_id": "a", "boot_id": "new", "firmware_mode": "recovery",
                  "app_version": "recovery-1", "capability_names": ["ota"]}
        service._authorized_lease = lambda *args: lease
        service._lease_public = lambda value: value
        store.update_maintenance_lease = lambda lease_id, **changes: {**lease, **changes}
        # No mutation methods are exposed by this hub, so accidental write
        # retries cannot pass the test.
        service.hub = SimpleNamespace(
            resume_maintenance_endpoint=AsyncMock(),
            list_devices=lambda: [{"device_id": "a", "endpoint": "usb:a"}],
            status=AsyncMock(side_effect=[first_error("closing session"), status]),
        )
        completed = await service.finish_maintenance("lease", "token", abort=False, timeout=1)
        assert completed["state"] == "released"
        assert completed["evidence_json"]["verification"] == status
        assert completed["evidence_json"]["observation_retries"] == 1
        assert service.hub.status.await_count == 2
        service.hub.resume_maintenance_endpoint.assert_awaited_once_with("usb:a")
        store.close()

    asyncio.run(scenario())


@pytest.mark.parametrize("condition", ["offline", "wrong_device", "old_boot", "wrong_version", "hung_status"])
def test_maintenance_timeout_does_not_accept_stale_or_foreign_identity(tmp_path, condition):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test")
        lease = {"lease_id": "lease", "device_id": "a", "state": "detached",
                 "endpoint": {"endpoint": "usb:a"}, "previous_boot_id": "old",
                 "expected_version": "recovery-1", "evidence": {}}
        service._authorized_lease = lambda *args: lease
        updates = []

        def update(lease_id, **changes):
            updates.append(changes)
            return {**lease, **changes}

        store.update_maintenance_lease = update

        async def status(device):
            if condition == "offline":
                raise ConnectionError("ESP-Iris serial link is closed")
            if condition == "hung_status":
                await asyncio.Event().wait()
            return {"device_id": "other" if condition == "wrong_device" else "a",
                    "boot_id": "old" if condition == "old_boot" else "new",
                    "app_version": "wrong" if condition == "wrong_version" else "recovery-1",
                    "firmware_mode": "recovery", "capability_names": ["ota"]}

        service.hub = SimpleNamespace(resume_maintenance_endpoint=AsyncMock(),
            list_devices=lambda: [{"device_id": "a", "endpoint": "usb:a"}], status=status)
        with pytest.raises(RuntimeError, match="identity verification did not complete"):
            await asyncio.wait_for(service.finish_maintenance("lease", "token", abort=False, timeout=0.25), 1)
        assert updates[-1]["state"] == "verification_failed"
        assert not any(item.get("state") == "released" for item in updates)
        service.hub.resume_maintenance_endpoint.assert_awaited_once()
        store.close()

    asyncio.run(scenario())



def test_http_maintenance_completion_persists_transient_reconnect_evidence(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test", demo=True)
        hub = DemoHub(service.on_device_event)
        service.attach_hub(hub)
        await hub.start()
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            response = await client.post("/v1/devices/demo-a1b2c3d4/maintenance-leases",
                                         json={"purpose": "recovery", "expected_version": "test-recovery"})
            assert response.status == 201
            lease = (await response.json())["lease"]
            device = hub._devices["demo-a1b2c3d4"]
            device.update(boot_id=device["boot_id"] + 1, firmware_mode="recovery", app_version="test-recovery")
            original_status = hub.status
            calls = []

            async def status(device_id):
                calls.append(device_id)
                if len(calls) == 1:
                    raise ConnectionError("ESP-Iris serial link is closed")
                return await original_status(device_id)

            hub.status = status
            response = await client.post(f"/v1/maintenance-leases/{lease['lease_id']}/complete",
                                         json={"timeout": 1}, headers={"X-Maintenance-Token": lease["token"]})
            assert response.status == 200, await response.text()
            persisted = store.maintenance_lease(lease["lease_id"])
            assert persisted["state"] == "released"
            assert persisted["evidence"]["observation_retries"] == 1
            assert persisted["evidence"]["verification"]["device_id"] == "demo-a1b2c3d4"
            assert calls == ["demo-a1b2c3d4"] * 2
        finally:
            await client.close()
            await hub.close()
            store.close()

    asyncio.run(scenario())
