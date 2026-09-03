from __future__ import annotations

import asyncio
import hashlib
import struct

import pytest

from iris_gateway.files import DeviceFiles, FileServiceError
from iris_gateway.protocol import Channel, FileStatus, FileType, Frame


def _metadata(kind: int, size: int, mtime_s: int, etag: int) -> bytes:
    return struct.pack("<BBHQQQ", kind, 0, 0, size, mtime_s, etag)


class FileSessionStub:
    def __init__(self) -> None:
        self.calls: list[tuple[int, int, bytes, int]] = []
        self.list_pages = [
            struct.pack("<HBB", FileStatus.OK, 0, 1)
            + struct.pack("<BBHQQQ", 1, 3, 0, 3, 100, 0x11)
            + b"one",
            struct.pack("<HBB", FileStatus.OK, 0, 1)
            + struct.pack("<BBHQQQ", 2, 3, 0, 0, 200, 0x22)
            + b"dir",
            struct.pack("<HBB", FileStatus.OK, 1, 0),
        ]

    async def _request(
        self,
        channel: int,
        type_: int,
        payload: bytes = b"",
        timeout: float = 10.0,
        *,
        stream_id: int = 0,
    ) -> Frame:
        del timeout
        self.calls.append((channel, type_, payload, stream_id))
        assert channel == Channel.FILE
        if type_ == FileType.VOLUMES_REQUEST:
            body = (
                struct.pack("<HHHHB3x", FileStatus.OK, 0, 1024, 255, 1)
                + struct.pack("<BBH", 3, 0, 7)
                + b"cfg"
            )
            return Frame(channel=channel, type=FileType.VOLUMES_RESPONSE, payload=body)
        if type_ == FileType.STAT_REQUEST:
            return Frame(
                channel=channel,
                type=FileType.STAT_RESPONSE,
                payload=struct.pack("<HH", FileStatus.OK, 0)
                + _metadata(1, 3, 100, 0x11),
            )
        if type_ == FileType.LIST_OPEN:
            return Frame(
                channel=channel,
                type=FileType.LIST_OPENED,
                stream_id=77,
                payload=struct.pack("<HHI", FileStatus.OK, 0, 77),
            )
        if type_ == FileType.LIST_NEXT:
            assert stream_id == 77
            return Frame(
                channel=channel,
                type=FileType.LIST_DATA,
                stream_id=77,
                payload=self.list_pages.pop(0),
            )
        if type_ == FileType.READ_OPEN:
            return Frame(
                channel=channel,
                type=FileType.READ_OPENED,
                stream_id=88,
                payload=struct.pack(
                    "<HHIQQQHH", FileStatus.OK, 0, 88, 3, 100, 0x11, 1024, 0
                ),
            )
        if type_ == FileType.READ:
            assert stream_id == 88
            assert payload == struct.pack("<QHH", 0, 3, 0)
            return Frame(
                channel=channel,
                type=FileType.DATA,
                stream_id=88,
                payload=struct.pack("<HHQQ", FileStatus.OK, 1, 0, 3) + b"abc",
            )
        assert type_ == FileType.CLOSE
        assert stream_id in {77, 88}
        return Frame(
            channel=channel,
            type=FileType.CLOSE_RESPONSE,
            stream_id=stream_id,
            payload=struct.pack("<HH", FileStatus.OK, 0),
        )


def test_file_read_service_contract() -> None:
    async def scenario() -> None:
        session = FileSessionStub()
        files = DeviceFiles(session)  # type: ignore[arg-type]

        volumes = await files.volumes()
        assert volumes == {
            "volumes": [
                {
                    "id": "cfg",
                    "capabilities": 7,
                    "capability_names": ["read", "list", "mtime"],
                }
            ],
            "chunk_max": 1024,
            "path_max": 255,
        }
        metadata = await files.stat("cfg", "app.json")
        assert metadata["kind"] == "file"
        assert metadata["size"] == 3
        assert session.calls[-1][2] == b"\x03\x00\x08\x00cfgapp.json"

        listing = await files.list_directory("cfg", "", limit=2)
        assert [entry["name"] for entry in listing["entries"]] == ["one", "dir"]
        assert listing["next_cursor"] is None
        assert listing["snapshot"] is False

        chunks = [chunk async for chunk in files.read_chunks("cfg", "app.json")]
        assert chunks == [b"abc"]
        assert [call[3] for call in session.calls if call[1] == FileType.CLOSE] == [77, 88]

    asyncio.run(scenario())


class FileWriteSessionStub:
    def __init__(self) -> None:
        self.calls: list[tuple[int, int, bytes, int]] = []
        self.data = bytearray()

    async def _request(
        self,
        channel: int,
        type_: int,
        payload: bytes = b"",
        timeout: float = 10.0,
        *,
        stream_id: int = 0,
    ) -> Frame:
        del timeout
        self.calls.append((channel, type_, payload, stream_id))
        assert channel == Channel.FILE
        if type_ == FileType.WRITE_OPEN:
            path_size = struct.unpack_from("<H", payload, 2)[0]
            trailer = 4 + 3 + path_size
            assert payload[:trailer] == b"\x03\x00\x08\x00cfgapp.json"
            assert struct.unpack_from("<QQHH", payload, trailer) == (6, 0, 0, 0)
            return Frame(
                channel=channel,
                type=FileType.WRITE_OPENED,
                stream_id=99,
                payload=struct.pack("<HHIHH", FileStatus.OK, 0, 99, 3, 0),
            )
        if type_ == FileType.WRITE:
            assert stream_id == 99
            offset, size, reserved = struct.unpack_from("<QHH", payload)
            assert reserved == 0
            assert offset == len(self.data)
            self.data.extend(payload[12:])
            assert len(payload[12:]) == size
            return Frame(
                channel=channel,
                type=FileType.WRITE_ACK,
                stream_id=99,
                payload=struct.pack("<HHQ", FileStatus.OK, 0, len(self.data)),
            )
        if type_ == FileType.COMMIT:
            assert stream_id == 99
            assert payload == hashlib.sha256(self.data).digest()
            return Frame(
                channel=channel,
                type=FileType.COMMIT_RESPONSE,
                stream_id=99,
                payload=struct.pack("<HH", FileStatus.OK, 0)
                + _metadata(1, len(self.data), 300, 0x33),
            )
        if type_ == FileType.MKDIR:
            return Frame(
                channel=channel,
                type=FileType.MKDIR_RESPONSE,
                payload=struct.pack("<HH", FileStatus.OK, 0)
                + _metadata(2, 0, 300, 0x44),
            )
        if type_ == FileType.DELETE:
            return Frame(
                channel=channel,
                type=FileType.DELETE_RESPONSE,
                payload=struct.pack("<HH", FileStatus.OK, 0),
            )
        if type_ == FileType.RENAME:
            return Frame(
                channel=channel,
                type=FileType.RENAME_RESPONSE,
                payload=struct.pack("<HH", FileStatus.OK, 0)
                + _metadata(1, len(self.data), 300, 0x55),
            )
        raise AssertionError(f"unexpected file request type {type_}")


def test_file_write_and_metadata_service_contract() -> None:
    async def chunks():
        yield b"ab"
        yield b"cdef"

    async def scenario() -> None:
        session = FileWriteSessionStub()
        files = DeviceFiles(session)  # type: ignore[arg-type]
        progress: list[tuple[int, int]] = []

        uploaded = await files.upload(
            "cfg",
            "app.json",
            chunks(),
            total_size=6,
            progress=lambda committed, total: _record_progress(
                progress, committed, total
            ),
        )
        assert bytes(session.data) == b"abcdef"
        assert uploaded["size"] == 6
        assert uploaded["sha256"] == hashlib.sha256(b"abcdef").hexdigest()
        assert progress == [(2, 6), (5, 6), (6, 6)]

        directory = await files.mkdir("cfg", "certs/new")
        assert directory["kind"] == "directory"
        assert await files.delete("cfg", "certs/new") == {
            "volume": "cfg",
            "path": "certs/new",
            "deleted": True,
        }
        renamed = await files.rename("cfg", "app.json", "app.old.json")
        assert renamed["source"] == "app.json"
        assert renamed["path"] == "app.old.json"
        rename_payload = next(
            call[2] for call in session.calls if call[1] == FileType.RENAME
        )
        assert rename_payload == (
            struct.pack("<BBHHH", 3, 0, 8, 12, 0)
            + b"cfgapp.jsonapp.old.json"
        )

    async def _record_progress(
        progress: list[tuple[int, int]], committed: int, total: int
    ) -> None:
        progress.append((committed, total))

    asyncio.run(scenario())


def test_write_timeout_reconciles_busy_then_committed_offset() -> None:
    class TimeoutWriteSessionStub(FileWriteSessionStub):
        def __init__(self) -> None:
            super().__init__()
            self.timed_out = False
            self.status_queries = 0

        async def _request(self, channel, type_, payload=b"", timeout=10.0, *, stream_id=0):
            if type_ == FileType.WRITE:
                frame = await super()._request(
                    channel, type_, payload, timeout, stream_id=stream_id
                )
                if not self.timed_out:
                    self.timed_out = True
                    raise TimeoutError
                return frame
            if type_ == FileType.WRITE_STATUS:
                self.calls.append((channel, type_, payload, stream_id))
                self.status_queries += 1
                if self.status_queries == 1:
                    return Frame(
                        channel=channel,
                        type=FileType.WRITE_STATUS_RESPONSE,
                        stream_id=stream_id,
                        payload=struct.pack("<HH", FileStatus.BUSY, 0),
                    )
                return Frame(
                    channel=channel,
                    type=FileType.WRITE_STATUS_RESPONSE,
                    stream_id=stream_id,
                    payload=struct.pack(
                        "<HHQQB3xHH",
                        FileStatus.OK,
                        0,
                        len(self.data),
                        6,
                        1,
                        FileStatus.OK,
                        0,
                    ),
                )
            return await super()._request(
                channel, type_, payload, timeout, stream_id=stream_id
            )

    async def chunks():
        yield b"abcdef"

    async def scenario() -> None:
        session = TimeoutWriteSessionStub()
        result = await DeviceFiles(session).upload(  # type: ignore[arg-type]
            "cfg", "app.json", chunks(), total_size=6
        )
        assert result["size"] == 6
        assert session.status_queries == 2
        assert [call[1] for call in session.calls].count(FileType.WRITE) == 2

    asyncio.run(scenario())


@pytest.mark.parametrize("path", ["/absolute", "a/../b", "a//b", "a\\b"])
def test_file_paths_must_be_canonical(path: str) -> None:
    with pytest.raises(ValueError):
        DeviceFiles._path_payload("cfg", path)


def test_stable_file_status_is_not_exposed_as_errno() -> None:
    class MissingSession:
        async def _request(self, *args, **kwargs) -> Frame:
            del args, kwargs
            return Frame(
                channel=Channel.FILE,
                type=FileType.STAT_RESPONSE,
                payload=struct.pack("<HH", FileStatus.NOT_FOUND, 0),
            )

    async def scenario() -> None:
        with pytest.raises(FileServiceError) as caught:
            await DeviceFiles(MissingSession()).stat("cfg", "missing")  # type: ignore[arg-type]
        assert caught.value.status is FileStatus.NOT_FOUND

    asyncio.run(scenario())
