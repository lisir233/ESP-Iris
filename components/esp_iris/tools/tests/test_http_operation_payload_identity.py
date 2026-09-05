from __future__ import annotations

import asyncio
from unittest.mock import AsyncMock

import pytest
from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.demo import DemoHub
from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.store import GatewayStore


@pytest.mark.parametrize("route,adapter,first,changed", [
    ("rpc/raw", "rpc", {"json": {"service_id": 1, "method_id": 1, "payload_text": "AAA"}},
     {"json": {"service_id": 1, "method_id": 1, "payload_text": "BBB"}}),
    ("rpc/system.echo", "rpc", {"json": {"params": {"value": "AAA"}}},
     {"json": {"params": {"value": "BBB"}}}),
    ("console", "rpc", {"json": {"line": "set AAA"}}, {"json": {"line": "set BBB"}}),
    ("input", "input_event", {"json": {"moves": [{"x": 1, "y": 2}]}},
     {"json": {"moves": [{"x": 2, "y": 1}]}}),
    ("mirror/start", "mirror_start", {"json": {"description": {"width": 100}}},
     {"json": {"description": {"width": 200}}}),
    ("audio", "audio_upload", {"data": b"AAA"}, {"data": b"BBB"}),
    ("screenshot", "screenshot", {"json": {"width": 64}}, {"json": {"width": 65}}),
    ("file", "file_upload", {"data": b"AAA", "params": {"volume": "cfg", "path": "identity.bin"}},
     {"data": b"BBB", "params": {"volume": "cfg", "path": "identity.bin"}}),
])
def test_same_size_different_content_conflicts_before_device_call(tmp_path, route, adapter, first, changed):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test", demo=True)
        hub = DemoHub(service.on_device_event)
        service.attach_hub(hub)
        await hub.start()
        call = AsyncMock(wraps=getattr(hub, adapter))
        setattr(hub, adapter, call)
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        method = "PUT" if route == "file" else "POST"
        url = "/v1/devices/demo-a1b2c3d4/" + route
        headers = {"X-Operation-ID": "same-operation"}
        try:
            response = await client.request(method, url, headers=headers, **first)
            assert response.status in {200, 201}, await response.text()
            await response.read()
            original = store.operation("same-operation")
            replay = await client.request(method, url, headers=headers, **first)
            assert replay.status in {200, 201}, await replay.text()
            await replay.read()
            conflict = await client.request(method, url, headers=headers, **changed)
            assert conflict.status == 409, await conflict.text()
            assert (await conflict.json())["error"]["code"] == "operation_id_conflict"
            assert call.await_count == 1
            assert store.operation("same-operation") == original
        finally:
            await client.close()
            await service.operations.close()
            await hub.close()
            store.close()

    asyncio.run(scenario())


def test_raw_rpc_identity_uses_decoded_bytes_not_input_encoding(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test", demo=True)
        hub = DemoHub(service.on_device_event)
        service.attach_hub(hub)
        await hub.start()
        hub.rpc = AsyncMock(wraps=hub.rpc)
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            for payload in ({"payload_text": "AAA"}, {"payload_hex": "414141"}, {"payload_base64": "QUFB"}):
                response = await client.post("/v1/devices/demo-a1b2c3d4/rpc/raw",
                    headers={"X-Operation-ID": "same"}, json={"service_id": 1, "method_id": 1, **payload})
                assert response.status == 200
                await response.read()
            assert hub.rpc.await_count == 1
        finally:
            await client.close()
            await service.operations.close()
            await hub.close()
            store.close()

    asyncio.run(scenario())
