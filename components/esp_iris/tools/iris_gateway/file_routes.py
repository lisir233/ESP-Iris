from __future__ import annotations

import pathlib
import re
import time
import uuid
from typing import Any
from urllib.parse import quote

from aiohttp import web

from .http_support import json_body, request_actor

FILE_UPLOAD_MAX_BYTES = 32 * 1024 * 1024


def _file_range(value: str | None, size: int) -> tuple[int, int, bool]:
    if not value:
        return 0, size, False
    match = re.fullmatch(r"bytes=(\d*)-(\d*)", value.strip())
    if match is None or size <= 0:
        raise web.HTTPRequestRangeNotSatisfiable(
            headers={"Content-Range": f"bytes */{size}"}
        )
    first, last = match.groups()
    if not first:
        suffix = int(last or "0")
        if suffix <= 0:
            raise web.HTTPRequestRangeNotSatisfiable(
                headers={"Content-Range": f"bytes */{size}"}
            )
        start = max(0, size - suffix)
        end = size - 1
    else:
        start = int(first)
        end = int(last) if last else size - 1
        if start >= size or end < start:
            raise web.HTTPRequestRangeNotSatisfiable(
                headers={"Content-Range": f"bytes */{size}"}
            )
        end = min(end, size - 1)
    return start, end - start + 1, True


def register_file_routes(app: web.Application, service: Any) -> None:
    def require_scope(request: web.Request, scope: str) -> web.Response | None:
        actor = request_actor(request)
        if actor.allows(scope):
            return None
        return web.json_response(
            {
                "error": {
                    "code": "insufficient_scope",
                    "message": f"file operation requires the {scope} scope",
                    "details": {"required_scope": scope},
                }
            },
            status=403,
        )

    def boolean_query(request: web.Request, name: str, default: bool = False) -> bool:
        value = request.query.get(name)
        if value is None:
            return default
        if value.lower() in {"1", "true", "yes"}:
            return True
        if value.lower() in {"0", "false", "no"}:
            return False
        raise ValueError(f"{name} query parameter must be true or false")

    def file_query(request: web.Request) -> tuple[str, str, str]:
        device_id = service.resolve_device(request.match_info["device_id"])
        volume = request.query.get("volume", "")
        path = request.query.get("path", "")
        if not volume:
            raise ValueError("file volume query parameter is required")
        return device_id, volume, path

    async def file_volumes(request: web.Request) -> web.Response:
        blocked = service.require_develop()
        if blocked is not None:
            return blocked
        blocked = require_scope(request, "files.read")
        if blocked is not None:
            return blocked
        device_id = service.resolve_device(request.match_info["device_id"])
        return web.json_response(await service.hub.file_volumes(device_id))

    async def file_stat(request: web.Request) -> web.Response:
        blocked = service.require_develop()
        if blocked is not None:
            return blocked
        blocked = require_scope(request, "files.read")
        if blocked is not None:
            return blocked
        device_id, volume, path = file_query(request)
        return web.json_response(await service.hub.file_stat(device_id, volume, path))

    async def file_list(request: web.Request) -> web.Response:
        blocked = service.require_develop()
        if blocked is not None:
            return blocked
        blocked = require_scope(request, "files.read")
        if blocked is not None:
            return blocked
        device_id, volume, path = file_query(request)
        cursor = int(request.query.get("cursor", "0"))
        limit = int(request.query.get("limit", "100"))
        if cursor < 0 or not 1 <= limit <= 200:
            raise ValueError("file list cursor/limit is outside the supported range")
        return web.json_response(
            await service.hub.file_list(
                device_id, volume, path, cursor=cursor, limit=limit
            )
        )

    async def file_download(request: web.Request) -> web.StreamResponse:
        blocked = service.require_develop()
        if blocked is not None:
            return blocked
        blocked = require_scope(request, "files.read")
        if blocked is not None:
            return blocked
        device_id, volume, path = file_query(request)
        metadata = await service.hub.file_stat(device_id, volume, path)
        if metadata.get("kind") != "file":
            raise ValueError("file download target is not a regular file")
        size = int(metadata["size"])
        offset, length, partial = _file_range(request.headers.get("Range"), size)
        etag = f'W/"{metadata["etag"]}"'
        if request.headers.get("If-None-Match") == etag and not partial:
            return web.Response(status=304, headers={"ETag": etag})
        filename = pathlib.PurePosixPath(path).name or "download.bin"
        headers = {
            "Accept-Ranges": "bytes",
            "Cache-Control": "no-store",
            "Content-Disposition": f"attachment; filename*=UTF-8''{quote(filename)}",
            "Content-Length": str(length),
            "Content-Type": "application/octet-stream",
            "ETag": etag,
            "X-ESP-Iris-Volume": volume,
        }
        if partial:
            headers["Content-Range"] = (
                f"bytes {offset}-{offset + length - 1}/{size}"
            )
        response = web.StreamResponse(
            status=206 if partial else 200, headers=headers
        )
        await response.prepare(request)
        async for chunk in service.hub.file_download(
            device_id, volume, path, offset=offset, length=length
        ):
            await response.write(chunk)
        await response.write_eof()
        actor = request_actor(request)
        service.store.add_audit(
            actor.kind,
            actor.name,
            "file.downloaded",
            {
                "device_id": device_id,
                "volume": volume,
                "path": path,
                "offset": offset,
                "bytes": length,
                "etag": metadata["etag"],
            },
        )
        return response

    async def file_upload(request: web.Request) -> web.Response:
        blocked = service.require_develop()
        if blocked is not None:
            return blocked
        blocked = require_scope(request, "files.write")
        if blocked is not None:
            return blocked
        device_id, volume, path = file_query(request)
        total_size = request.content_length
        if total_size is None:
            raise ValueError("file upload requires a Content-Length header")
        if total_size > FILE_UPLOAD_MAX_BYTES:
            raise web.HTTPRequestEntityTooLarge(
                max_size=FILE_UPLOAD_MAX_BYTES, actual_size=total_size
            )
        overwrite = boolean_query(request, "overwrite")
        if_match = request.headers.get("If-Match")
        operation_id = request.headers.get("X-Operation-ID") or str(uuid.uuid4())
        last_progress = -1
        last_update = 0.0

        async def progress(committed: int, expected: int) -> None:
            nonlocal last_progress, last_update
            permille = 1000 if expected == 0 else committed * 1000 // expected
            now = time.monotonic()
            if (
                permille < 1000
                and permille - last_progress < 10
                and now - last_update < 0.5
            ):
                return
            last_progress = permille
            last_update = now
            await service.operations.progress(
                operation_id,
                stage="transferring",
                progress_permille=permille,
                committed_bytes=committed,
                total_bytes=expected,
            )

        async def call() -> dict[str, Any]:
            await progress(0, total_size)
            return await service.hub.file_upload(
                device_id,
                volume,
                path,
                request.content.iter_chunked(64 * 1024),
                total_size=total_size,
                overwrite=overwrite,
                if_match=if_match,
                progress=progress,
            )

        operation, result, created = await service.operations.execute(
            device_id,
            request_actor(request),
            "file.upload",
            {
                "volume": volume,
                "path": path,
                "size": total_size,
                "overwrite": overwrite,
                "if_match": if_match,
            },
            call,
            operation_id=operation_id,
        )
        return web.json_response(
            {
                "operation": operation,
                "file": result,
                "idempotent_reuse": not created,
            },
            status=(
                200
                if isinstance(result, dict) and result.get("replaced")
                else 201
            ),
            headers={"X-Operation-ID": operation_id},
        )

    async def file_mkdir(request: web.Request) -> web.Response:
        blocked = service.require_develop()
        if blocked is not None:
            return blocked
        blocked = require_scope(request, "files.write")
        if blocked is not None:
            return blocked
        body = await json_body(request)
        device_id = service.resolve_device(request.match_info["device_id"])
        volume = str(body.get("volume", ""))
        path = str(body.get("path", ""))
        operation, result, created = await service.operations.execute(
            device_id,
            request_actor(request),
            "file.mkdir",
            {"volume": volume, "path": path},
            lambda: service.hub.file_mkdir(device_id, volume, path),
            operation_id=request.headers.get("X-Operation-ID"),
        )
        return web.json_response(
            {
                "operation": operation,
                "directory": result,
                "idempotent_reuse": not created,
            },
            status=201,
            headers={"X-Operation-ID": operation["operation_id"]},
        )

    async def file_delete(request: web.Request) -> web.Response:
        blocked = service.require_develop()
        if blocked is not None:
            return blocked
        blocked = require_scope(request, "files.delete")
        if blocked is not None:
            return blocked
        device_id, volume, path = file_query(request)
        operation, result, created = await service.operations.execute(
            device_id,
            request_actor(request),
            "file.delete",
            {"volume": volume, "path": path},
            lambda: service.hub.file_delete(device_id, volume, path),
            operation_id=request.headers.get("X-Operation-ID"),
        )
        return web.json_response(
            {
                "operation": operation,
                "file": result,
                "idempotent_reuse": not created,
            },
            headers={"X-Operation-ID": operation["operation_id"]},
        )

    async def file_rename(request: web.Request) -> web.Response:
        blocked = service.require_develop()
        if blocked is not None:
            return blocked
        blocked = require_scope(request, "files.write")
        if blocked is not None:
            return blocked
        body = await json_body(request)
        device_id = service.resolve_device(request.match_info["device_id"])
        volume = str(body.get("volume", ""))
        source = str(body.get("source", ""))
        destination = str(body.get("destination", ""))
        operation, result, created = await service.operations.execute(
            device_id,
            request_actor(request),
            "file.rename",
            {
                "volume": volume,
                "source": source,
                "destination": destination,
            },
            lambda: service.hub.file_rename(
                device_id, volume, source, destination
            ),
            operation_id=request.headers.get("X-Operation-ID"),
        )
        return web.json_response(
            {
                "operation": operation,
                "file": result,
                "idempotent_reuse": not created,
            },
            headers={"X-Operation-ID": operation["operation_id"]},
        )

    app.router.add_get("/v1/devices/{device_id}/files/volumes", file_volumes)
    app.router.add_get("/v1/devices/{device_id}/files/stat", file_stat)
    app.router.add_get("/v1/devices/{device_id}/files", file_list)
    app.router.add_get("/v1/devices/{device_id}/file", file_download)
    app.router.add_put("/v1/devices/{device_id}/file", file_upload)
    app.router.add_delete("/v1/devices/{device_id}/file", file_delete)
    app.router.add_post("/v1/devices/{device_id}/directories", file_mkdir)
    app.router.add_post("/v1/devices/{device_id}/file-rename", file_rename)


__all__ = ["register_file_routes"]
