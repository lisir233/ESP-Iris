"""Bounded System Update transactions over an established device session.

This module owns message sequencing and timeout reconciliation. Discovery,
link lifecycle and request correlation remain in ``DeviceSession``; archive
authentication and release policy remain in ``system_update`` and Gateway.
"""

from __future__ import annotations

import asyncio
import contextlib
import secrets
import struct
from typing import Any, Awaitable, Callable, Dict, Protocol

from .protocol import (
    Capability,
    Channel,
    Frame,
    ProtocolError,
    SystemUpdateType,
)
from .system_update import SystemUpdateBundle


SYSTEM_UPDATE_REQUEST_TIMEOUT = 60.0


class ReadyInfo(Protocol):
    capabilities: int


WaitReady = Callable[[float], Awaitable[ReadyInfo]]
Request = Callable[[int, int, bytes, float], Awaitable[Frame]]
ProgressCallback = Callable[[Dict[str, Any]], Awaitable[None]]


def decode_system_update_status(payload: bytes) -> dict[str, Any]:
    if len(payload) != 36:
        raise ProtocolError("invalid system-update status response")
    operation_id = payload[:16]
    job_id = struct.unpack_from("<I", payload, 16)[0]
    phase = payload[20]
    component_count = payload[21]
    completed_components = payload[22]
    active_component_id = payload[23]
    received, total, result = struct.unpack_from("<IIi", payload, 24)
    phase_names = {
        0: "idle",
        1: "prepared",
        2: "receiving",
        3: "component_verified",
        4: "committing",
        5: "committed",
        6: "cancelled",
        7: "failed",
    }
    if phase not in phase_names or completed_components > component_count:
        raise ProtocolError("invalid system-update status fields")
    return {
        "operation_id": operation_id.hex(),
        "job_id": job_id,
        "phase": phase_names[phase],
        "phase_id": phase,
        "component_count": component_count,
        "completed_components": completed_components,
        "active_component_id": active_component_id,
        "bytes_received": received,
        "bytes_total": total,
        "result": result,
    }


async def system_update_status(
    request: Request, *, timeout: float = 10.0
) -> dict[str, Any]:
    frame = await request(
        Channel.SYSTEM_UPDATE, SystemUpdateType.STATUS, b"", timeout
    )
    if frame.type != SystemUpdateType.STATUS_RESPONSE:
        raise ProtocolError("unexpected system-update status response")
    return decode_system_update_status(frame.payload)


async def system_update_inventory(
    wait_ready: WaitReady,
    request: Request,
    *,
    timeout: float = 10.0,
) -> dict[str, Any]:
    info = await wait_ready(timeout)
    if not info.capabilities & Capability.SYSTEM_INVENTORY:
        raise RuntimeError("device does not advertise system-inventory support")
    frame = await request(
        Channel.SYSTEM_UPDATE, SystemUpdateType.INVENTORY, b"", timeout
    )
    if frame.type != SystemUpdateType.INVENTORY_RESPONSE or len(frame.payload) != 92:
        raise ProtocolError("unexpected system-update inventory response")
    flags, layout_version = struct.unpack_from("<II", frame.payload)
    last_result = struct.unpack_from("<i", frame.payload, 88)[0]
    return {
        "flags": flags,
        "layout_version": layout_version,
        "bootloader_sha256": frame.payload[8:40].hex() if flags & (1 << 0) else "",
        "partition_table_sha256": (
            frame.payload[40:72].hex() if flags & (1 << 1) else ""
        ),
        "last_operation_id": (
            frame.payload[72:88].hex() if flags & (1 << 2) else ""
        ),
        "last_result": last_result,
    }


async def perform_system_update(
    wait_ready: WaitReady,
    request: Request,
    bundle: SystemUpdateBundle,
    *,
    operation_id: bytes | None = None,
    timeout: float = SYSTEM_UPDATE_REQUEST_TIMEOUT,
    progress_callback: ProgressCallback | None = None,
) -> dict[str, Any]:
    """Transfer and commit one authenticated multi-image update plan."""

    info = await wait_ready(timeout)
    if not info.capabilities & Capability.SYSTEM_UPDATE:
        raise RuntimeError("device does not advertise system-update support")
    identifier = operation_id or secrets.token_bytes(16)
    if len(identifier) != 16 or identifier == bytes(16):
        raise ValueError("system-update operation ID must be 16 non-zero bytes")
    if len(bundle.manifest_bytes) > 0xFFFF or len(bundle.signature) > 0xFFFF:
        raise ValueError("system-update manifest or signature is too large")
    begin = (
        identifier
        + struct.pack(
            "<HHBBH",
            len(bundle.manifest_bytes),
            len(bundle.signature),
            len(bundle.components),
            0,
            0,
        )
        + bundle.manifest_sha256
        + bundle.manifest_bytes
        + bundle.signature
    )
    try:
        frame = await request(
            Channel.SYSTEM_UPDATE, SystemUpdateType.BEGIN, begin, timeout
        )
    except (asyncio.TimeoutError, TimeoutError):
        status = await system_update_status(request, timeout=timeout)
        if (
            status["operation_id"] != identifier.hex()
            or status["phase"] != "prepared"
            or status["completed_components"] != 0
            or status["active_component_id"] != 0
            or status["component_count"] != len(bundle.components)
        ):
            raise TimeoutError(
                f"system-update begin timeout; device status is {status}"
            ) from None
        frame = await request(
            Channel.SYSTEM_UPDATE, SystemUpdateType.BEGIN, begin, timeout
        )
    if frame.type != SystemUpdateType.BEGIN_RESPONSE or len(frame.payload) != 24:
        raise ProtocolError("unexpected system-update begin response")
    returned_id = frame.payload[:16]
    job_id, chunk_size = struct.unpack_from("<IH", frame.payload, 16)
    component_count = frame.payload[22]
    if (
        returned_id != identifier
        or job_id == 0
        or chunk_size == 0
        or component_count != len(bundle.components)
    ):
        raise ProtocolError("invalid system-update begin response")

    total_bundle_bytes = sum(item.size for item in bundle.components)
    transferred = 0
    completion_evidence = "commit_response"
    try:
        for component_index, component in enumerate(bundle.components):
            descriptor = (
                identifier
                + bytes([component.id, int(component.kind)])
                + struct.pack(
                    "<HII", component.flags, component.target_offset, component.size
                )
                + component.sha256
            )
            opened = await request(
                Channel.SYSTEM_UPDATE,
                SystemUpdateType.COMPONENT_BEGIN,
                descriptor,
                timeout,
            )
            if (
                opened.type != SystemUpdateType.COMPONENT_BEGIN_RESPONSE
                or len(opened.payload) != 24
                or opened.payload[:16] != identifier
                or opened.payload[16] != component.id
                or opened.payload[17] != int(component.kind)
            ):
                raise ProtocolError("invalid system-update component response")
            accepted_chunk, accepted_size = struct.unpack_from(
                "<HI", opened.payload, 18
            )
            if accepted_chunk == 0 or accepted_size != component.size:
                raise ProtocolError("device rejected system-update component bounds")
            effective_chunk = min(chunk_size, accepted_chunk)
            offset = 0
            while offset < component.size:
                chunk = component.data[offset : offset + effective_chunk]
                message = (
                    identifier
                    + bytes([component.id, 0])
                    + b"\0\0"
                    + struct.pack("<I", offset)
                    + chunk
                )
                try:
                    response = await request(
                        Channel.SYSTEM_UPDATE,
                        SystemUpdateType.DATA,
                        message,
                        timeout,
                    )
                except (asyncio.TimeoutError, TimeoutError):
                    status = await system_update_status(request, timeout=timeout)
                    if (
                        status["operation_id"] != identifier.hex()
                        or status["active_component_id"] != component.id
                        or status["phase"] != "receiving"
                    ):
                        raise TimeoutError(
                            "system-update data timeout; "
                            f"device status is {status}"
                        ) from None
                    received = int(status["bytes_received"])
                    if received not in (offset, offset + len(chunk)):
                        raise ProtocolError(
                            f"system update resumed at unexpected offset {received}"
                        )
                    offset = received
                    continue
                if (
                    response.type != SystemUpdateType.DATA_RESPONSE
                    or len(response.payload) != 24
                    or response.payload[:16] != identifier
                    or response.payload[16] != component.id
                    or response.payload[17] != 0
                ):
                    raise ProtocolError("invalid system-update data response")
                progress, received = struct.unpack_from(
                    "<HI", response.payload, 18
                )
                if received != offset + len(chunk):
                    raise ProtocolError("invalid system-update committed offset")
                offset = received
                if progress_callback is not None:
                    await progress_callback(
                        {
                            "stage": "transferring",
                            "job_id": job_id,
                            "operation_id": identifier.hex(),
                            "component_id": component.id,
                            "component_kind": component.kind.name.lower(),
                            "component_index": component_index,
                            "component_count": component_count,
                            "bytes_received": transferred + offset,
                            "bytes_total": total_bundle_bytes,
                            "progress_permille": progress,
                        }
                    )
            ended = await request(
                Channel.SYSTEM_UPDATE,
                SystemUpdateType.COMPONENT_END,
                identifier + bytes([component.id, 0, 0, 0]),
                timeout,
            )
            if (
                ended.type != SystemUpdateType.COMPONENT_END_RESPONSE
                or len(ended.payload) != 24
                or ended.payload[:16] != identifier
                or ended.payload[16] != component.id
                or ended.payload[17] != component_index + 1
                or struct.unpack_from("<i", ended.payload, 20)[0] != 0
            ):
                raise ProtocolError("invalid system-update component completion")
            transferred += component.size
        if progress_callback is not None:
            await progress_callback(
                {
                    "stage": "committing",
                    "job_id": job_id,
                    "operation_id": identifier.hex(),
                    "bytes_received": transferred,
                    "bytes_total": total_bundle_bytes,
                    "progress_permille": 950,
                }
            )
        try:
            committed = await request(
                Channel.SYSTEM_UPDATE,
                SystemUpdateType.COMMIT,
                identifier,
                timeout,
            )
        except (asyncio.TimeoutError, TimeoutError):
            status = await system_update_status(request, timeout=timeout)
            if (
                status["operation_id"] != identifier.hex()
                or status["phase"] != "committed"
                or status["result"] != 0
            ):
                raise TimeoutError(
                    f"system-update commit timeout; device status is {status}"
                ) from None
            completion_evidence = "device_status"
            committed = None
        except (ConnectionError, OSError):
            completion_evidence = "session_close"
            committed = None
    except BaseException:
        with contextlib.suppress(Exception):
            await request(
                Channel.SYSTEM_UPDATE,
                SystemUpdateType.CANCEL,
                identifier,
                timeout,
            )
        raise
    if committed is not None:
        if (
            committed.type != SystemUpdateType.COMMIT_RESPONSE
            or len(committed.payload) != 24
            or committed.payload[:16] != identifier
        ):
            raise ProtocolError("invalid system-update commit response")
        returned_job, result = struct.unpack_from("<Ii", committed.payload, 16)
        if returned_job != job_id or result != 0:
            raise RuntimeError(
                f"system update failed with device error 0x{result & 0xFFFFFFFF:08x}"
            )
    return {
        "operation_id": identifier.hex(),
        "job_id": job_id,
        "manifest_sha256": bundle.manifest_sha256.hex(),
        "target_layout_sha256": bundle.target_layout_sha256,
        "components": [item.as_dict() for item in bundle.components],
        "bytes": transferred,
        "restart_required": True,
        "completion_evidence": completion_evidence,
    }


__all__ = [
    "decode_system_update_status",
    "perform_system_update",
    "system_update_inventory",
    "system_update_status",
]
