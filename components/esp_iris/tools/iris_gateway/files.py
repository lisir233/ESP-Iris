from __future__ import annotations

import asyncio
import contextlib
import hashlib
import re
import struct
from collections.abc import AsyncIterable, AsyncIterator, Awaitable, Callable
from typing import TYPE_CHECKING, Any

from .compat import remove_prefix
from .protocol import Channel, FileStatus, FileType, Frame, ProtocolError

if TYPE_CHECKING:
    from .session import DeviceSession


VOLUME_ID = re.compile(r"^[A-Za-z0-9_-]{1,15}$")
FILE_KIND = {1: "file", 2: "directory"}
VOLUME_CAPABILITIES = {
    1: "read",
    2: "list",
    4: "mtime",
    8: "write",
    16: "delete",
    32: "mkdir",
    64: "rename",
    128: "atomic_replace",
    256: "hash",
}
WRITE_OVERWRITE = 1 << 0
WRITE_IF_MATCH = 1 << 1
WRITE_ACTIVE = 1
WRITE_COMMITTED = 2
WRITE_ABORTED = 3


class FileServiceError(RuntimeError):
    def __init__(self, status: FileStatus, operation: str) -> None:
        self.status = status
        self.operation = operation
        super().__init__(f"file {operation} failed: {status.name.lower()}")


def file_error_http_status(status: FileStatus) -> int:
    return {
        FileStatus.INVALID_ARGUMENT: 400,
        FileStatus.NOT_FOUND: 404,
        FileStatus.NOT_DIRECTORY: 400,
        FileStatus.NOT_FILE: 400,
        FileStatus.READ_ONLY: 403,
        FileStatus.BUSY: 409,
        FileStatus.NO_MEMORY: 503,
        FileStatus.IO: 502,
        FileStatus.NOT_SUPPORTED: 501,
        FileStatus.CONFLICT: 409,
        FileStatus.EXISTS: 409,
        FileStatus.NOT_EMPTY: 409,
        FileStatus.NO_SPACE: 507,
        FileStatus.HASH_MISMATCH: 422,
    }.get(status, 502)


class DeviceFiles:
    def __init__(self, session: DeviceSession) -> None:
        self._session = session

    @staticmethod
    def _path_payload(volume: str, path: str) -> bytes:
        if not VOLUME_ID.fullmatch(volume):
            raise ValueError("file volume ID must contain 1 to 15 ASCII identifier bytes")
        if path.startswith("/") or path.endswith("/") or "\\" in path:
            raise ValueError("file path must be an unrooted canonical relative path")
        parts = path.split("/") if path else []
        if any(part in {"", ".", ".."} for part in parts):
            raise ValueError("file path contains an invalid component")
        encoded_volume = volume.encode("ascii")
        encoded_path = path.encode("utf-8")
        if len(encoded_path) > 255 or any(value < 0x20 or value == 0x7F for value in encoded_path):
            raise ValueError("file path exceeds 255 bytes or contains control bytes")
        return struct.pack("<BBH", len(encoded_volume), 0, len(encoded_path)) + encoded_volume + encoded_path

    @staticmethod
    def _checked(frame: Frame, type_: FileType, operation: str, minimum: int = 4) -> bytes:
        if frame.channel != Channel.FILE or frame.type != type_ or len(frame.payload) < 4:
            raise ProtocolError(f"unexpected file {operation} response")
        raw_status = struct.unpack_from("<H", frame.payload)[0]
        try:
            status = FileStatus(raw_status)
        except ValueError as exc:
            raise ProtocolError(f"unknown file status {raw_status}") from exc
        if status is not FileStatus.OK:
            raise FileServiceError(status, operation)
        if len(frame.payload) < minimum:
            raise ProtocolError(f"short file {operation} response")
        return frame.payload

    async def volumes(self) -> dict[str, Any]:
        frame = await self._session._request(Channel.FILE, FileType.VOLUMES_REQUEST)
        payload = self._checked(frame, FileType.VOLUMES_RESPONSE, "volumes", 12)
        chunk_max, path_max = struct.unpack_from("<HH", payload, 4)
        count = payload[8]
        offset = 12
        volumes = []
        for _ in range(count):
            if offset + 4 > len(payload):
                raise ProtocolError("truncated file volume record")
            name_size, reserved, capabilities = struct.unpack_from("<BBH", payload, offset)
            offset += 4
            if reserved != 0 or name_size == 0 or offset + name_size > len(payload):
                raise ProtocolError("invalid file volume record")
            name = payload[offset : offset + name_size].decode("ascii")
            offset += name_size
            volumes.append(
                {
                    "id": name,
                    "capabilities": capabilities,
                    "capability_names": [
                        label for bit, label in VOLUME_CAPABILITIES.items() if capabilities & bit
                    ],
                }
            )
        if offset != len(payload) or chunk_max == 0 or path_max == 0:
            raise ProtocolError("invalid file volumes response")
        return {"volumes": volumes, "chunk_max": chunk_max, "path_max": path_max}

    @staticmethod
    def _metadata(
        payload: bytes, offset: int = 4, *, list_entry: bool = False
    ) -> dict[str, Any]:
        if len(payload) < offset + 28:
            raise ProtocolError("short file metadata")
        kind, reserved_byte, reserved, size, mtime_s, etag = struct.unpack_from(
            "<BBHQQQ", payload, offset
        )
        if (
            (not list_entry and reserved_byte != 0)
            or reserved != 0
            or kind not in FILE_KIND
        ):
            raise ProtocolError("invalid file metadata")
        return {
            "kind": FILE_KIND[kind],
            "size": size,
            "mtime_s": mtime_s,
            "etag": f"{etag:016x}",
        }

    async def stat(self, volume: str, path: str) -> dict[str, Any]:
        frame = await self._session._request(
            Channel.FILE, FileType.STAT_REQUEST, self._path_payload(volume, path)
        )
        payload = self._checked(frame, FileType.STAT_RESPONSE, "stat", 32)
        if len(payload) != 32:
            raise ProtocolError("invalid file stat response")
        return {"volume": volume, "path": path, **self._metadata(payload)}

    async def list_directory(
        self, volume: str, path: str, *, cursor: int = 0, limit: int = 100
    ) -> dict[str, Any]:
        if cursor < 0 or not 1 <= limit <= 200:
            raise ValueError("file list cursor/limit is outside the supported range")
        opened = await self._session._request(
            Channel.FILE, FileType.LIST_OPEN, self._path_payload(volume, path)
        )
        payload = self._checked(opened, FileType.LIST_OPENED, "list open", 8)
        if len(payload) != 8:
            raise ProtocolError("invalid file list open response")
        stream_id = struct.unpack_from("<I", payload, 4)[0]
        if stream_id == 0 or opened.stream_id != stream_id:
            raise ProtocolError("invalid file list stream ID")
        entries: list[dict[str, Any]] = []
        seen = 0
        ended = False
        try:
            while len(entries) <= limit:
                frame = await self._session._request(
                    Channel.FILE,
                    FileType.LIST_NEXT,
                    stream_id=stream_id,
                )
                page = self._checked(frame, FileType.LIST_DATA, "list next")
                if frame.stream_id != stream_id or page[3] not in (0, 1):
                    raise ProtocolError("invalid file list page")
                ended = bool(page[2] & 1)
                if page[3] == 1:
                    metadata = self._metadata(page, list_entry=True)
                    name_size = page[5]
                    if len(page) != 32 + name_size:
                        raise ProtocolError("invalid file list entry")
                    name = page[32:].decode("utf-8")
                    if seen >= cursor:
                        entries.append({"name": name, **metadata})
                    seen += 1
                elif len(page) != 4:
                    raise ProtocolError("invalid empty file list page")
                if ended or len(entries) > limit:
                    break
        finally:
            with contextlib.suppress(Exception):
                await self._session._request(
                    Channel.FILE, FileType.CLOSE, stream_id=stream_id
                )
        has_more = len(entries) > limit
        if has_more:
            entries.pop()
        return {
            "volume": volume,
            "path": path,
            "entries": entries,
            "cursor": cursor,
            "next_cursor": cursor + len(entries) if has_more else None,
            "snapshot": False,
        }

    async def read_chunks(
        self,
        volume: str,
        path: str,
        *,
        offset: int = 0,
        length: int | None = None,
    ) -> AsyncIterator[bytes]:
        if offset < 0 or length is not None and length < 0:
            raise ValueError("file read range must be nonnegative")
        opened = await self._session._request(
            Channel.FILE, FileType.READ_OPEN, self._path_payload(volume, path)
        )
        payload = self._checked(opened, FileType.READ_OPENED, "read open", 36)
        if len(payload) != 36:
            raise ProtocolError("invalid file read open response")
        stream_id = struct.unpack_from("<I", payload, 4)[0]
        total_size, _, _, chunk_max, reserved = struct.unpack_from("<QQQHH", payload, 8)
        if stream_id == 0 or opened.stream_id != stream_id or chunk_max == 0 or reserved != 0:
            raise ProtocolError("invalid file read stream metadata")
        if offset > total_size:
            raise ValueError("file read offset exceeds file size")
        end = total_size if length is None else min(total_size, offset + length)
        cursor = offset
        try:
            while cursor < end:
                maximum = min(chunk_max, end - cursor, 0xFFFF)
                frame = await self._session._request(
                    Channel.FILE,
                    FileType.READ,
                    struct.pack("<QHH", cursor, maximum, 0),
                    stream_id=stream_id,
                )
                data_payload = self._checked(frame, FileType.DATA, "read", 20)
                returned_offset, returned_total = struct.unpack_from("<QQ", data_payload, 4)
                data = data_payload[20:]
                if (
                    frame.stream_id != stream_id
                    or returned_offset != cursor
                    or returned_total != total_size
                    or not data
                    or len(data) > maximum
                ):
                    raise ProtocolError("invalid file data response")
                if cursor + len(data) > end:
                    data = data[: end - cursor]
                cursor += len(data)
                yield data
        finally:
            with contextlib.suppress(Exception):
                await self._session._request(
                    Channel.FILE, FileType.CLOSE, stream_id=stream_id
                )

    @staticmethod
    def _etag_value(value: str | None) -> int:
        if value is None:
            return 0
        normalized = remove_prefix(value.strip(), "W/").strip('"')
        if not re.fullmatch(r"[0-9a-fA-F]{16}", normalized):
            raise ValueError("file etag must contain exactly 16 hexadecimal characters")
        return int(normalized, 16)

    async def write_status(self, stream_id: int) -> dict[str, Any]:
        frame = await self._session._request(
            Channel.FILE,
            FileType.WRITE_STATUS,
            stream_id=stream_id,
        )
        payload = self._checked(
            frame, FileType.WRITE_STATUS_RESPONSE, "write status", 28
        )
        if len(payload) != 28 or frame.stream_id != stream_id:
            raise ProtocolError("invalid file write status response")
        committed, expected = struct.unpack_from("<QQ", payload, 4)
        state = payload[20]
        reserved = payload[21:24]
        raw_result, tail_reserved = struct.unpack_from("<HH", payload, 24)
        if state not in {WRITE_ACTIVE, WRITE_COMMITTED, WRITE_ABORTED} or any(
            reserved
        ) or tail_reserved != 0:
            raise ProtocolError("invalid file write status fields")
        try:
            result = FileStatus(raw_result)
        except ValueError as exc:
            raise ProtocolError(f"unknown file write result {raw_result}") from exc
        return {
            "stream_id": stream_id,
            "committed": committed,
            "expected": expected,
            "state": {
                WRITE_ACTIVE: "active",
                WRITE_COMMITTED: "committed",
                WRITE_ABORTED: "aborted",
            }[state],
            "result": result,
        }

    async def _reconcile_write_status(self, stream_id: int) -> dict[str, Any]:
        for attempt in range(12):
            try:
                return await self.write_status(stream_id)
            except FileServiceError as exc:
                if exc.status is not FileStatus.BUSY or attempt == 11:
                    raise
                await asyncio.sleep(min(0.05 * (attempt + 1), 0.5))
        raise AssertionError("unreachable file write reconciliation state")

    async def upload(
        self,
        volume: str,
        path: str,
        chunks: AsyncIterable[bytes],
        *,
        total_size: int,
        overwrite: bool = False,
        if_match: str | None = None,
        progress: Callable[[int, int], Awaitable[None]] | None = None,
    ) -> dict[str, Any]:
        if total_size < 0 or total_size >= 1 << 63:
            raise ValueError("file upload size is outside the supported range")
        flags = WRITE_OVERWRITE if overwrite else 0
        if_match_value = self._etag_value(if_match)
        if if_match is not None:
            flags |= WRITE_IF_MATCH
        opened = await self._session._request(
            Channel.FILE,
            FileType.WRITE_OPEN,
            self._path_payload(volume, path)
            + struct.pack("<QQHH", total_size, if_match_value, flags, 0),
            timeout=10.0,
        )
        payload = self._checked(opened, FileType.WRITE_OPENED, "write open", 12)
        if len(payload) != 12:
            raise ProtocolError("invalid file write open response")
        stream_id, chunk_max, reserved = struct.unpack_from("<IHH", payload, 4)
        if (
            stream_id == 0
            or opened.stream_id != stream_id
            or chunk_max == 0
            or reserved != 0
        ):
            raise ProtocolError("invalid file write stream metadata")
        digest = hashlib.sha256()
        committed = 0
        complete = False
        try:
            async for chunk in chunks:
                data = bytes(chunk)
                if not data:
                    continue
                if committed + len(data) > total_size:
                    raise ValueError("HTTP upload exceeds its declared content length")
                for start in range(0, len(data), chunk_max):
                    part = data[start : start + chunk_max]
                    target = committed + len(part)
                    acknowledged = False
                    for attempt in range(2):
                        try:
                            frame = await self._session._request(
                                Channel.FILE,
                                FileType.WRITE,
                                struct.pack("<QHH", committed, len(part), 0)
                                + part,
                                timeout=10.0,
                                stream_id=stream_id,
                            )
                            ack = self._checked(
                                frame, FileType.WRITE_ACK, "write", 12
                            )
                            if (
                                len(ack) != 12
                                or frame.stream_id != stream_id
                                or struct.unpack_from("<Q", ack, 4)[0] != target
                            ):
                                raise ProtocolError("invalid file write acknowledgement")
                            acknowledged = True
                            break
                        except (asyncio.TimeoutError, TimeoutError):
                            status = await self._reconcile_write_status(stream_id)
                            if status["committed"] == target:
                                acknowledged = True
                                break
                            if status["committed"] != committed or attempt != 0:
                                raise
                    if not acknowledged:
                        raise TimeoutError("file write acknowledgement was not recovered")
                    digest.update(part)
                    committed = target
                    if progress is not None:
                        await progress(committed, total_size)
            if committed != total_size:
                raise ValueError("HTTP upload ended before its declared content length")
            try:
                frame = await self._session._request(
                    Channel.FILE,
                    FileType.COMMIT,
                    digest.digest(),
                    timeout=15.0,
                    stream_id=stream_id,
                )
            except (asyncio.TimeoutError, TimeoutError):
                status = await self._reconcile_write_status(stream_id)
                if status["state"] == "committed" and status["result"] is FileStatus.OK:
                    complete = True
                    return {
                        **await self.stat(volume, path),
                        "sha256": digest.hexdigest(),
                        "replaced": overwrite,
                    }
                if status["state"] != "active":
                    raise FileServiceError(status["result"], "commit")
                frame = await self._session._request(
                    Channel.FILE,
                    FileType.COMMIT,
                    digest.digest(),
                    timeout=15.0,
                    stream_id=stream_id,
                )
            result = self._checked(
                frame, FileType.COMMIT_RESPONSE, "commit", 32
            )
            if len(result) != 32 or frame.stream_id != stream_id:
                raise ProtocolError("invalid file commit response")
            complete = True
            return {
                "volume": volume,
                "path": path,
                **self._metadata(result),
                "sha256": digest.hexdigest(),
                "replaced": overwrite,
            }
        finally:
            if not complete:
                with contextlib.suppress(Exception):
                    await self._session._request(
                        Channel.FILE,
                        FileType.ABORT,
                        timeout=10.0,
                        stream_id=stream_id,
                    )

    async def mkdir(self, volume: str, path: str) -> dict[str, Any]:
        frame = await self._session._request(
            Channel.FILE, FileType.MKDIR, self._path_payload(volume, path)
        )
        payload = self._checked(frame, FileType.MKDIR_RESPONSE, "mkdir", 32)
        if len(payload) != 32:
            raise ProtocolError("invalid file mkdir response")
        return {"volume": volume, "path": path, **self._metadata(payload)}

    async def delete(self, volume: str, path: str) -> dict[str, Any]:
        frame = await self._session._request(
            Channel.FILE, FileType.DELETE, self._path_payload(volume, path)
        )
        payload = self._checked(frame, FileType.DELETE_RESPONSE, "delete")
        if len(payload) != 4:
            raise ProtocolError("invalid file delete response")
        return {"volume": volume, "path": path, "deleted": True}

    async def rename(
        self, volume: str, source: str, destination: str
    ) -> dict[str, Any]:
        self._path_payload(volume, source)
        self._path_payload(volume, destination)
        encoded_volume = volume.encode("ascii")
        encoded_source = source.encode("utf-8")
        encoded_destination = destination.encode("utf-8")
        payload = (
            struct.pack(
                "<BBHHH",
                len(encoded_volume),
                0,
                len(encoded_source),
                len(encoded_destination),
                0,
            )
            + encoded_volume
            + encoded_source
            + encoded_destination
        )
        frame = await self._session._request(Channel.FILE, FileType.RENAME, payload)
        response = self._checked(frame, FileType.RENAME_RESPONSE, "rename", 32)
        if len(response) != 32:
            raise ProtocolError("invalid file rename response")
        return {
            "volume": volume,
            "source": source,
            "path": destination,
            **self._metadata(response),
        }


__all__ = ["DeviceFiles", "FileServiceError", "file_error_http_status"]
