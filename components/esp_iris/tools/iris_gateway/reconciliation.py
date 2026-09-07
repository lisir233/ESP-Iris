"""Append-only read-only observations of uncertain operations; never replay writes."""

from __future__ import annotations

import asyncio
import json
import math
import time
import uuid
from typing import Any

from .contracts import GatewayHub
from .security import Actor
from .store import GatewayStore


def health_timeout(status: dict[str, Any], override: float | None = None) -> float:
    seconds = float(override) if override is not None else float(status.get("health_timeout_ms", 45000)) / 1000
    if not math.isfinite(seconds) or not 1 <= seconds <= 600:
        raise ValueError("OTA health timeout must be between 1 and 600 seconds")
    return seconds


def reconciliation_records(store: GatewayStore, operation_id: str) -> list[dict[str, Any]]:
    if store.operation(operation_id) is None:
        raise KeyError(operation_id)
    return [json.loads(row[0]) for row in store.db.execute(
        "SELECT record_json FROM operation_reconciliations WHERE operation_id=? ORDER BY rowid",
        (operation_id,),
    ).fetchall()]


def _target_components_match(params: dict[str, Any], progress: dict[str, Any],
                             status: dict[str, Any], inventory: dict[str, Any]) -> bool:
    components = params.get("bundle", {}).get("components")
    if not isinstance(components, list):
        return False  # Legacy history without the target contract cannot prove success.
    for component in components:
        kind = component.get("kind")
        if kind == "bootloader":
            expected = component.get("sha256")
            if not expected or inventory.get("bootloader_sha256") != expected:
                return False
        elif kind == "application":
            expected = progress.get("target_application") or {}
            # BIN hash binds the ELF/project identity captured before the write to
            # the exact component in the persisted request, not a later artifact.
            if (not expected.get("elf_sha256") or not component.get("sha256")
                    or expected.get("sha256") != component["sha256"]
                    or status.get("project_name") != expected.get("project_name")
                    or str(status.get("firmware_sha256", "")).lower()
                    != str(expected["elf_sha256"]).lower()):
                return False
        elif kind == "recovery":
            expected = progress.get("target_recovery") or {}
            if (not expected.get("elf_sha256") or not component.get("sha256")
                    or expected.get("sha256") != component["sha256"]
                    or status.get("project_name") != expected.get("project_name")
                    or str(status.get("firmware_sha256", "")).lower()
                    != str(expected["elf_sha256"]).lower()):
                return False
    return True


async def reconcile_operation(
    store: GatewayStore, hub: GatewayHub, operation_id: str, actor: Actor
) -> dict[str, Any]:
    operation = store.operation(operation_id)
    if operation is None:
        raise KeyError(operation_id)
    if operation["status"] not in {"outcome_unknown", "interrupted"}:
        raise ValueError("only uncertain or interrupted operations can be reconciled")
    device_id = str(operation["device_id"])
    record: dict[str, Any] = {
        "reconciliation_id": str(uuid.uuid4()), "operation_id": operation_id,
        "device_id": device_id, "actor": actor.as_dict(), "observed_ns": time.time_ns(),
        "outcome": "outcome_unknown", "replayed": False, "evidence": {},
        "reason": "insufficient current evidence",
    }
    try:
        # Bounded, live RPCs only. Cached identity and reconnect alone are not proof.
        status = await asyncio.wait_for(hub.status(device_id), 10)
        record["evidence"]["status"] = status
        if status.get("device_id") != device_id or status.get("boot_id") is None:
            record["reason"] = "live device identity is missing or does not match"
        else:
            progress = operation.get("progress") or {}
            writer_boot = progress.get("writer_boot_id", progress.get("previous_boot_id"))
            boot = status["boot_id"]
            events = store.latest_events(device_id=device_id, categories=["device"], limit=5000)
            healthy = next((event for event in reversed(events)
                            if event.get("event_name") == "healthy" and event.get("boot_id") == boot
                            and event.get("host_receive_ns", 0) >= operation["created_ns"]), None)
            if healthy is not None:
                record["evidence"]["healthy_event"] = healthy
            params = operation["params"]
            if operation["action"] == "firmware.ota":
                image = params.get("image", {})
                # Reconciliation always uses ELF content identity, even if the
                # original operation used the weaker version acceptance option.
                matches = bool(image.get("elf_sha256")) and (
                    str(status.get("firmware_sha256", "")).lower() == str(image["elf_sha256"]).lower()
                    and status.get("project_name") == image.get("project_name")
                )
                if (matches and healthy and status.get("firmware_mode") == "normal"
                        and writer_boot is not None and boot != writer_boot):
                    record.update(outcome="observed_success", reason="expected firmware is healthy on a new boot")
            elif operation["action"] == "firmware.system_update":
                inventory = await asyncio.wait_for(hub.system_update_inventory(device_id), 10)
                record["evidence"]["inventory"] = inventory
                after = await asyncio.wait_for(hub.status(device_id), 10)
                record["evidence"]["status_after_inventory"] = after
                if after.get("device_id") != device_id or after.get("boot_id") != boot:
                    raise RuntimeError("device rebooted during inventory observation")
                try:
                    wire_id = uuid.UUID(operation_id).hex
                except ValueError:
                    wire_id = uuid.uuid5(uuid.NAMESPACE_URL, operation_id).hex
                receipt_matches = inventory.get("last_operation_id") == wire_id
                components = params.get("bundle", {}).get("components", [])
                expected_mode = (
                    "recovery"
                    if any(item.get("kind") == "recovery" for item in components)
                    else "normal"
                )
                if receipt_matches and inventory.get("last_result") not in (None, 0):
                    record.update(outcome="observed_failure", reason="matching device receipt reports failure")
                elif (receipt_matches and inventory.get("last_result") == 0 and healthy
                      and status.get("firmware_mode") == expected_mode
                      and after.get("firmware_mode") == expected_mode
                      and writer_boot is not None and boot != writer_boot
                      and inventory.get("partition_table_sha256") == params.get("bundle", {}).get("target_layout_sha256")
                      and _target_components_match(params, progress, after, inventory)):
                    record["evidence"]["target_application"] = progress.get("target_application")
                    record["evidence"]["target_recovery"] = progress.get("target_recovery")
                    record.update(outcome="observed_success", reason="matching commit receipt, layout and healthy new boot")
            else:
                record["reason"] = "action has no durable proof contract; live observation retained"
    except (OSError, ConnectionError, LookupError, RuntimeError, asyncio.TimeoutError) as exc:
        record["reason"] = "live observation unavailable: " + type(exc).__name__
    store.db.execute(
        "INSERT INTO operation_reconciliations VALUES (?, ?, ?)",
        (record["reconciliation_id"], operation_id, json.dumps(record, ensure_ascii=False)),
    )
    store.db.commit()
    return record
