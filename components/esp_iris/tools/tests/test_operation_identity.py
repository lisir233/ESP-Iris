from __future__ import annotations

import asyncio

import pytest
from aiohttp.test_utils import TestClient, TestServer

from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.operation_identity import OperationConflict
from iris_gateway.operations import OperationManager
from iris_gateway.security import Actor
from iris_gateway.store import GatewayStore


@pytest.mark.parametrize("submit", [False, True])
def test_id_binds_request_and_actor_across_restart(tmp_path, submit):
    async def scenario():
        store = GatewayStore(tmp_path)
        actor = Actor("agent", "alice")
        calls = []

        async def call():
            calls.append(1)
            return "done"

        manager = OperationManager(store, lambda event: asyncio.sleep(0))
        invoke = manager.submit if submit else manager.execute
        await invoke("a", actor, "ota", {"b": b"image", "a": 1}, call, operation_id="one")
        if manager._tasks:
            await asyncio.gather(*manager._tasks)
        await manager.close()
        store.close()
        store = GatewayStore(tmp_path)
        manager = OperationManager(store, lambda event: asyncio.sleep(0))
        invoke = manager.submit if submit else manager.execute
        result = await invoke("a", actor, "ota", {"a": 1, "b": b"image"}, call, operation_id="one")
        assert result[-1] is False
        for device, who, action, params in [
            ("b", actor, "ota", {"a": 1, "b": b"image"}),
            ("a", Actor("agent", "bob"), "ota", {"a": 1, "b": b"image"}),
            ("a", Actor("agent", "alice", frozenset()), "ota", {"a": 1, "b": b"image"}),
            ("a", actor, "restart", {"a": 1, "b": b"image"}),
            ("a", actor, "ota", {"a": 1, "b": b"changed"}),
        ]:
            with pytest.raises(OperationConflict):
                await invoke(device, who, action, params, call, operation_id="one")
        assert calls == [1]
        # A migrated row stays readable, but cannot authorize a replay.
        store.db.execute("UPDATE operations SET request_fingerprint=NULL")
        store.db.commit()
        with pytest.raises(OperationConflict, match="legacy"):
            await invoke("a", actor, "ota", {"a": 1, "b": b"image"}, call, operation_id="one")
        await manager.close()
        store.close()

    asyncio.run(scenario())


def test_parallel_submissions_execute_once_and_conflict(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        manager = OperationManager(store, lambda event: asyncio.sleep(0))
        calls = []

        async def call():
            calls.append(1)
            await asyncio.sleep(0)

        results = await asyncio.gather(*[
            manager.submit("a", Actor("agent", "a"), "ota", {"value": value}, call,
                           operation_id="same") for value in [1, 1, 2]
        ], return_exceptions=True)
        await asyncio.sleep(0)
        assert calls == [1]
        assert isinstance(results[2], OperationConflict)
        await manager.close()
        store.close()

    asyncio.run(scenario())


def test_http_conflict_is_409(tmp_path):
    async def scenario():
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test")
        store.remember_device({"device_id": "a"})
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            await service.operations.execute(
                "a", Actor("local", "Unauthenticated local client"), "different", {},
                lambda: asyncio.sleep(0), operation_id="same")
            response = await client.post("/v1/devices/a/restart", json={"delay_ms": 250},
                                         headers={"X-Operation-ID": "same"})
            assert response.status == 409
            assert "operation_id_conflict" in await response.text()
        finally:
            await client.close()
            await service.operations.close()
            store.close()

    asyncio.run(scenario())


def test_separate_sqlite_connections_register_once(tmp_path):
    from concurrent.futures import ThreadPoolExecutor
    from threading import Barrier

    GatewayStore(tmp_path).close()
    barrier = Barrier(2)
    request = {"operation_id": "race", "device_id": "a", "actor_type": "agent",
               "actor_name": "test", "action": "ota", "params": {},
               "status": "queued", "created_ns": 1}

    def insert():
        store = GatewayStore(tmp_path)
        try:
            barrier.wait(timeout=5)
            return store.create_operation(request)[1]
        finally:
            store.close()

    with ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(insert) for _ in range(2)]
        assert sorted(future.result(timeout=10) for future in futures) == [False, True]


def test_v4_database_keeps_legacy_request_unverifiable(tmp_path):
    import sqlite3

    from iris_gateway.migrations import MIGRATIONS

    db = sqlite3.connect(tmp_path / "gateway.sqlite3")
    for migration in MIGRATIONS[:4]:
        migration(db)
    db.execute("PRAGMA user_version=4")
    db.execute("INSERT INTO operations(operation_id, device_id, actor_type, actor_name, "
               "action, params_json, status, created_ns) VALUES ('old','a','agent','test','ota','{}','succeeded',1)")
    db.commit()
    db.close()
    store = GatewayStore(tmp_path)
    assert store.operation("old")["request_fingerprint"] is None
    with pytest.raises(OperationConflict, match="legacy"):
        store.create_operation({"operation_id": "old", "device_id": "a", "actor_type": "agent",
                                "actor_name": "test", "action": "ota", "params": {},
                                "status": "queued", "created_ns": 2})
    store.close()
