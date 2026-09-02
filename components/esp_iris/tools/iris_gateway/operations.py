from __future__ import annotations

import asyncio
import dataclasses
import hashlib
import time
import uuid
from collections import defaultdict
from collections.abc import Awaitable, Callable
from typing import Any

from .observability import MetricsRegistry
from .security import Actor
from .state_machine import TERMINAL_OPERATION_STATES, operation_transition
from .store import GatewayStore


class OperationCancelled(RuntimeError):
    pass


class OperationOutcomeUnknown(RuntimeError):
    pass


class DeviceMaintenance(RuntimeError):
    """Raised when a device is reserved for host-side maintenance."""


@dataclasses.dataclass(slots=True)
class _Pending:
    operation_id: str
    device_id: str
    cancelled: bool = False
    running: bool = False


def _safe_value(value: Any) -> Any:
    if isinstance(value, bytes):
        return {
            "bytes": len(value),
            "sha256": hashlib.sha256(value).hexdigest(),
        }
    if isinstance(value, dict):
        return {str(key): _safe_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_safe_value(item) for item in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


class OperationManager:
    def __init__(
        self,
        store: GatewayStore,
        event_sink: Callable[[dict[str, Any]], Awaitable[None]],
        metrics: MetricsRegistry | None = None,
    ) -> None:
        self.store = store
        self.event_sink = event_sink
        self._locks: defaultdict[str, asyncio.Lock] = defaultdict(asyncio.Lock)
        self._pending: dict[str, _Pending] = {}
        self._tasks: set[asyncio.Task[Any]] = set()
        self._submission_ids: set[str] = set()
        self._maintenance_pending: set[str] = set()
        self._maintenance_active: set[str] = set()
        self.metrics = metrics or MetricsRegistry()

    def maintenance_state(self, device_id: str) -> str | None:
        if device_id in self._maintenance_active:
            return "active"
        if device_id in self._maintenance_pending:
            return "pending"
        return None

    async def acquire_maintenance(self, device_id: str, timeout: float) -> None:
        """Drain earlier work and atomically block later work for one device."""

        if self.maintenance_state(device_id) is not None:
            raise DeviceMaintenance(f"device {device_id} already has maintenance pending")
        self._maintenance_pending.add(device_id)
        lock = self._locks[device_id]
        try:
            await asyncio.wait_for(lock.acquire(), timeout=max(0.1, timeout))
        except BaseException:
            self._maintenance_pending.discard(device_id)
            raise
        self._maintenance_pending.discard(device_id)
        self._maintenance_active.add(device_id)

    def restore_maintenance(self, device_id: str) -> None:
        """Restore the gate after a Gateway restart without opening the endpoint."""

        self._maintenance_active.add(device_id)

    def release_maintenance(self, device_id: str) -> None:
        self._maintenance_pending.discard(device_id)
        self._maintenance_active.discard(device_id)
        lock = self._locks.get(device_id)
        if lock is not None and lock.locked():
            lock.release()

    def _transition(
        self, operation_id: str, status: str, **changes: Any
    ) -> dict[str, Any]:
        current = self.store.operation(operation_id)
        if current is None:
            raise KeyError(operation_id)
        target = operation_transition(str(current["status"]), status)
        operation = self.store.update_operation(
            operation_id, status=target.value, **changes
        )
        self.metrics.increment(f"operations.transition.{target.value}")
        if target in TERMINAL_OPERATION_STATES:
            started = operation.get("started_ns") or operation.get("created_ns")
            finished = operation.get("finished_ns")
            if started and finished and finished >= started:
                self.metrics.observe(
                    "operations.duration_seconds", (finished - started) / 1_000_000_000
                )
        return operation

    def queue_state(self, device_id: str) -> dict[str, Any]:
        items = [
            item
            for item in self._pending.values()
            if item.device_id == device_id
        ]
        return {
            "device_id": device_id,
            "maintenance": self.maintenance_state(device_id),
            "running": [item.operation_id for item in items if item.running],
            "queued": [
                item.operation_id
                for item in items
                if not item.running and not item.cancelled
            ],
        }

    async def stage(self, operation_id: str, status: str) -> dict[str, Any]:
        if status not in {
            "running",
            "preserving_evidence",
            "entering_recovery",
            "waiting_recovery",
            "recovery_connected",
            "preparing_ota",
            "erasing",
            "validating_plan",
            "transferring",
            "verifying",
            "committing",
            "waiting_device",
            "reconnecting",
        }:
            raise ValueError(f"invalid active operation stage: {status}")
        operation = self._transition(operation_id, status)
        await self._emit(operation)
        return operation

    async def progress(
        self,
        operation_id: str,
        *,
        stage: str,
        progress_permille: int,
        **details: Any,
    ) -> dict[str, Any]:
        progress_permille = max(0, min(int(progress_permille), 1000))
        operation = self._transition(
            operation_id,
            stage,
            progress_json={
                "stage": stage,
                "progress_permille": progress_permille,
                "updated_ns": time.time_ns(),
                **_safe_value(details),
            },
        )
        await self._emit(operation)
        return operation

    async def submit(
        self,
        device_id: str,
        actor: Actor,
        action: str,
        params: dict[str, Any],
        call: Callable[[], Awaitable[Any]],
        *,
        operation_id: str | None = None,
        serialized: bool = True,
        result_summary: Callable[[Any], Any] | None = None,
    ) -> tuple[dict[str, Any], bool]:
        """Queue an operation and return immediately while it runs in background."""

        if self.maintenance_state(device_id) is not None:
            raise DeviceMaintenance(
                f"device {device_id} is reserved for host-side maintenance"
            )
        operation_id = operation_id or str(uuid.uuid4())
        existing = self.store.operation(operation_id)
        if existing is not None:
            return existing, False
        if operation_id in self._submission_ids:
            for _ in range(10):
                await asyncio.sleep(0)
                existing = self.store.operation(operation_id)
                if existing is not None:
                    return existing, False
            raise RuntimeError("concurrent operation submission was not registered")
        self._submission_ids.add(operation_id)
        task = asyncio.create_task(
            self.execute(
                device_id,
                actor,
                action,
                params,
                call,
                operation_id=operation_id,
                serialized=serialized,
                result_summary=result_summary,
            ),
            name=f"esp-iris-operation-{operation_id}",
        )
        self._tasks.add(task)

        def completed(done: asyncio.Task[Any]) -> None:
            self._tasks.discard(done)
            self._submission_ids.discard(operation_id)
            # Retrieve the exception so a failed background request does not
            # become an unhandled asyncio task. Its durable operation row is
            # the public result channel.
            if not done.cancelled():
                done.exception()

        task.add_done_callback(completed)
        await asyncio.sleep(0)
        operation = self.store.operation(operation_id)
        if operation is None:
            raise RuntimeError("background operation was not registered")
        return operation, True

    async def cancel_queued(self) -> int:
        count = 0
        for pending in tuple(self._pending.values()):
            if not pending.running and not pending.cancelled:
                pending.cancelled = True
                count += 1
                operation = self._transition(
                    pending.operation_id,
                    "cancelled",
                    error="cancelled when gateway entered observe mode",
                    finished_ns=time.time_ns(),
                )
                await self._emit(operation)
        return count

    async def close(self) -> None:
        tasks = tuple(self._tasks)
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)

    async def execute(
        self,
        device_id: str,
        actor: Actor,
        action: str,
        params: dict[str, Any],
        call: Callable[[], Awaitable[Any]],
        *,
        operation_id: str | None = None,
        serialized: bool = True,
        result_summary: Callable[[Any], Any] | None = None,
    ) -> tuple[dict[str, Any], Any, bool]:
        if self.maintenance_state(device_id) is not None:
            raise DeviceMaintenance(
                f"device {device_id} is reserved for host-side maintenance"
            )
        operation_id = operation_id or str(uuid.uuid4())
        queue_position = sum(
            1
            for item in self._pending.values()
            if item.device_id == device_id and not item.cancelled
        )
        operation, created = self.store.create_operation(
            {
                "operation_id": operation_id,
                "device_id": device_id,
                "actor_type": actor.kind,
                "actor_name": actor.name,
                "action": action,
                "params": _safe_value(params),
                "status": "queued" if serialized else "running",
                "created_ns": time.time_ns(),
                "queue_position": queue_position,
            }
        )
        if not created:
            return operation, operation.get("result"), False
        pending = _Pending(operation_id, device_id)
        self._pending[operation_id] = pending
        await self._emit(operation)
        try:
            if serialized:
                async with self._locks[device_id]:
                    if pending.cancelled:
                        raise OperationCancelled(
                            "operation cancelled before it reached the device"
                        )
                    completed, result = await self._run(
                        pending, call, result_summary=result_summary
                    )
                    return completed, result, True
            completed, result = await self._run(
                pending, call, result_summary=result_summary
            )
            return completed, result, True
        finally:
            self._pending.pop(operation_id, None)

    async def _run(
        self,
        pending: _Pending,
        call: Callable[[], Awaitable[Any]],
        *,
        result_summary: Callable[[Any], Any] | None,
    ) -> tuple[dict[str, Any], Any]:
        pending.running = True
        operation = self._transition(
            pending.operation_id,
            "running",
            started_ns=time.time_ns(),
            queue_position=0,
        )
        await self._emit(operation)
        try:
            result = await call()
        except asyncio.CancelledError:
            operation = self._transition(
                pending.operation_id,
                "interrupted",
                error="gateway task was interrupted",
                finished_ns=time.time_ns(),
            )
            await self._emit(operation)
            raise
        except OperationCancelled:
            raise
        except (OperationOutcomeUnknown, TimeoutError) as exc:
            operation = self._transition(
                pending.operation_id,
                "outcome_unknown",
                error=str(exc) or "device outcome could not be established",
                finished_ns=time.time_ns(),
            )
            await self._emit(operation)
            raise
        except Exception as exc:
            operation = self._transition(
                pending.operation_id,
                "failed",
                error=str(exc),
                finished_ns=time.time_ns(),
            )
            await self._emit(operation)
            raise
        summary = result_summary(result) if result_summary else result
        current = self.store.operation(pending.operation_id)
        progress = current.get("progress") if current else None
        if isinstance(progress, dict):
            progress = {
                **progress,
                "stage": "succeeded",
                "progress_permille": 1000,
                "updated_ns": time.time_ns(),
            }
        operation = self._transition(
            pending.operation_id,
            "succeeded",
            result_json=_safe_value(summary),
            progress_json=progress,
            finished_ns=time.time_ns(),
        )
        await self._emit(operation)
        return operation, result

    async def _emit(self, operation: dict[str, Any]) -> None:
        await self.event_sink(
            {
                "kind": "operation",
                "device_id": operation["device_id"],
                "host_receive_wall_ns": time.time_ns(),
                "operation": operation,
            }
        )


__all__ = ["OperationCancelled", "OperationManager", "OperationOutcomeUnknown"]
