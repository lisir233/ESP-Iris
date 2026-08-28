"""Gateway closed-loop orchestration for authenticated System Update."""

from __future__ import annotations

import asyncio
import uuid
from collections.abc import Awaitable, Callable
from typing import Any

from .contracts import GatewayHub
from .firmware import inspect_firmware_image
from .operations import OperationManager, OperationOutcomeUnknown
from .system_update import SystemUpdateBundle, SystemUpdateComponentKind

PreserveCoreDump = Callable[[str], Awaitable[dict[str, Any] | None]]
ValidateIdentity = Callable[[dict[str, Any], dict[str, Any], str], dict[str, str]]


async def run_system_update(
    hub: GatewayHub,
    operations: OperationManager,
    preserve_coredump: PreserveCoreDump,
    validate_identity: ValidateIdentity,
    device_id: str,
    bundle: SystemUpdateBundle,
    operation_id: str,
    *,
    validation_mode: str,
) -> dict[str, Any]:
    """Run a recovery-only update and prove the resulting system state."""

    before = await hub.status(device_id)
    previous_boot = before.get("boot_id")
    preserved_coredump = await preserve_coredump(device_id)
    if preserved_coredump is not None:
        await operations.progress(
            operation_id,
            stage="preserving_evidence",
            progress_permille=20,
            coredump=preserved_coredump,
        )
    if before.get("firmware_mode") != "recovery":
        await operations.progress(
            operation_id,
            stage="entering_recovery",
            progress_permille=30,
            previous_boot_id=previous_boot,
        )
        try:
            await hub.enter_recovery(device_id)
        except (ConnectionError, OSError):
            pass
        deadline = asyncio.get_running_loop().time() + 30
        recovery_status: dict[str, Any] | None = None
        while asyncio.get_running_loop().time() < deadline:
            await asyncio.sleep(0.25)
            try:
                candidate = await hub.status(device_id)
            except (
                ConnectionError,
                OSError,
                KeyError,
                LookupError,
                RuntimeError,
            ):
                continue
            if (
                candidate.get("firmware_mode") == "recovery"
                and candidate.get("boot_id") != previous_boot
            ):
                recovery_status = candidate
                break
        if recovery_status is None:
            raise OperationOutcomeUnknown(
                "factory recovery did not reconnect for system update"
            )
    else:
        recovery_status = before

    writer_boot = recovery_status.get("boot_id")
    inventory_before = await hub.system_update_inventory(device_id)
    source_layout = str(inventory_before.get("partition_table_sha256", ""))
    if source_layout not in bundle.source_layout_sha256:
        raise ValueError(
            "device partition-table SHA-256 is not authorized by the bundle"
        )
    await operations.progress(
        operation_id,
        stage="validating_plan",
        progress_permille=50,
        source_inventory=inventory_before,
    )
    try:
        wire_operation_id = uuid.UUID(operation_id).bytes
    except ValueError:
        wire_operation_id = uuid.uuid5(uuid.NAMESPACE_URL, operation_id).bytes
    last_progress = -10

    async def report_progress(progress: dict[str, Any]) -> None:
        nonlocal last_progress
        device_progress = int(progress.get("progress_permille", 0))
        if device_progress < last_progress + 10 and device_progress < 950:
            return
        last_progress = device_progress
        await operations.progress(
            operation_id,
            stage=str(progress.get("stage", "transferring")),
            progress_permille=50 + device_progress * 800 // 1000,
            device_progress_permille=device_progress,
            component_id=progress.get("component_id"),
            component_kind=progress.get("component_kind"),
            bytes_received=progress.get("bytes_received", 0),
            bytes_total=progress.get("bytes_total", 0),
            job_id=progress.get("job_id"),
        )

    queue = hub.subscribe(device_id)
    try:
        result = await hub.system_update(
            device_id,
            bundle,
            operation_id=wire_operation_id,
            progress_callback=report_progress,
        )
        await operations.progress(
            operation_id,
            stage="waiting_device",
            progress_permille=875,
            writer_boot_id=writer_boot,
        )
        if result.get("completion_evidence") != "session_close":
            try:
                await hub.restart(device_id, 250)
            except (ConnectionError, OSError, KeyError):
                pass
        deadline = asyncio.get_running_loop().time() + 60
        new_boot: Any = None
        healthy = False
        while asyncio.get_running_loop().time() < deadline:
            try:
                event = await asyncio.wait_for(queue.get(), 1.0)
            except TimeoutError:
                continue
            event_boot = event.get("boot_id")
            if event_boot is not None and event_boot != writer_boot:
                new_boot = event_boot
            if event.get("event_name") == "healthy" and event_boot == new_boot:
                healthy = True
                break
        if new_boot is None or not healthy:
            raise RuntimeError(
                "system update did not reconnect as a healthy application "
                "within 60 seconds"
            )
        status = await hub.status(device_id)
        inventory_after = await hub.system_update_inventory(device_id)
    finally:
        hub.unsubscribe(device_id, queue)

    if inventory_after.get("partition_table_sha256") != bundle.target_layout_sha256:
        raise RuntimeError("post-update partition-table SHA-256 does not match")
    if inventory_after.get("last_operation_id") != wire_operation_id.hex():
        raise RuntimeError("post-update sysmeta operation ID does not match")
    if int(inventory_after.get("last_result", -1)) != 0:
        raise RuntimeError("post-update sysmeta records a failed commit")
    bootloader = next(
        (
            item
            for item in bundle.components
            if item.kind is SystemUpdateComponentKind.BOOTLOADER
        ),
        None,
    )
    if bootloader is not None and (
        inventory_after.get("bootloader_sha256") != bootloader.sha256.hex()
    ):
        raise RuntimeError("post-update bootloader SHA-256 does not match")
    application = next(
        (
            item
            for item in bundle.components
            if item.kind is SystemUpdateComponentKind.APPLICATION
        ),
        None,
    )
    application_validation: dict[str, Any] | None = None
    if application is not None:
        metadata = inspect_firmware_image(application.data).as_dict()
        application_validation = validate_identity(
            status, metadata, validation_mode
        )
    return {
        **result,
        "validated": True,
        "healthy": healthy,
        "previous_boot_id": previous_boot,
        "writer_boot_id": writer_boot,
        "boot_id": new_boot,
        "source_inventory": inventory_before,
        "target_inventory": inventory_after,
        "application_validation": application_validation,
        "preserved_coredump": preserved_coredump,
    }


__all__ = ["run_system_update"]
