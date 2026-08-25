import asyncio

import pytest

from iris_gateway.observability import MetricsRegistry
from iris_gateway.operations import OperationManager
from iris_gateway.security import Actor
from iris_gateway.state_machine import StateTransitionError
from iris_gateway.store import GatewayStore


def test_operation_manager_serializes_transitions_and_records_duration(tmp_path) -> None:
    async def scenario() -> None:
        store = GatewayStore(tmp_path)
        metrics = MetricsRegistry()
        events = []

        async def sink(event):
            events.append(event)

        manager = OperationManager(store, sink, metrics)
        operation, result, created = await manager.execute(
            "device-a",
            Actor("agent", "test"),
            "rpc.raw",
            {},
            lambda: asyncio.sleep(0, result={"ok": True}),
            operation_id="op-1",
        )
        assert created and result == {"ok": True}
        assert operation["status"] == "succeeded"
        assert [item["operation"]["status"] for item in events] == [
            "queued",
            "running",
            "succeeded",
        ]
        snapshot = metrics.snapshot()
        assert snapshot["counters"]["operations.transition.succeeded"] == 1
        assert snapshot["distributions"]["operations.duration_seconds"]["count"] == 1
        with pytest.raises(StateTransitionError):
            manager._transition("op-1", "running")
        store.close()

    asyncio.run(scenario())


def test_observe_transition_cancels_queued_operation(tmp_path) -> None:
    async def scenario() -> None:
        store = GatewayStore(tmp_path)

        async def sink(event):
            del event

        manager = OperationManager(store, sink)
        store.create_operation(
            {
                "operation_id": "queued",
                "device_id": "device-a",
                "actor_type": "agent",
                "actor_name": "test",
                "action": "rpc.raw",
                "params": {},
                "status": "queued",
                "created_ns": 1,
            }
        )
        from iris_gateway.operations import _Pending

        manager._pending["queued"] = _Pending("queued", "device-a")
        assert await manager.cancel_queued() == 1
        assert store.operation("queued")["status"] == "cancelled"
        store.close()

    asyncio.run(scenario())
