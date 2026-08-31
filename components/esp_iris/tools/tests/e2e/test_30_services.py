from __future__ import annotations

import hashlib
import struct
from typing import Any

import pytest

from iris_gateway.files import DeviceFiles, FileServiceError
from iris_gateway.protocol import Channel, FileStatus, FileType

from .contracts import (
    MEDIA_CONFIGURE_METHOD,
    MEDIA_CONFIGURE_V1,
    POINTER_V1,
    STATE_METHOD,
    TEST_SERVICE_ID,
    FixtureState,
)
from .helpers import run
from .raw import RawIrisSession

pytestmark = [
    pytest.mark.iris_e2e,
    pytest.mark.iris_stage(3),
    pytest.mark.firmware_profile("services_usb"),
]


async def _open_usb(iris_board) -> RawIrisSession:
    raw = RawIrisSession(iris_board.discover_application_port())
    await raw.open()
    return raw


async def _chunks(*values: bytes):
    for value in values:
        yield value


def _file_status(payload: bytes) -> FileStatus:
    return FileStatus(struct.unpack_from("<H", payload)[0])


def test_screenshot_and_pointer_are_observed_on_device(
    iris_board, firmware_profile
) -> None:
    assert firmware_profile == "services_usb"

    async def scenario() -> None:
        raw = await _open_usb(iris_board)
        try:
            assert raw.session is not None
            description, pixels = await raw.session.screenshot()
            assert description == {
                "x": 0,
                "y": 0,
                "width": 2,
                "height": 2,
                "stride": 4,
                "format": 1,
                "quality": 0,
            }
            assert pixels == bytes.fromhex("00f8e0071f00ffff")
            with pytest.raises(RuntimeError, match="device error"):
                await raw.request(
                    Channel.SCREEN,
                    3,
                    struct.pack("<IHH", 8, 1, 0),
                    stream_id=1,
                )

            begin = POINTER_V1.pack(0, 0, -100, 900, 0, 1)
            moved = POINTER_V1.pack(1, 0, 100, 200, 0, 2)
            end = POINTER_V1.pack(2, 0, 700, -5, 0, 3)
            assert POINTER_V1.unpack(await raw.session.rpc(0x1001, 1, begin))[2:4] == (
                0,
                479,
            )
            await raw.session.rpc(0x1001, 1, moved)
            normalized_end = await raw.session.rpc(0x1001, 1, end)
            assert POINTER_V1.unpack(normalized_end)[2:4] == (479, 0)
            state = FixtureState.decode(
                await raw.session.rpc(TEST_SERVICE_ID, STATE_METHOD)
            )
            assert state.pointer_count == 3
            assert state.last_pointer == normalized_end
        finally:
            await raw.close()

    run(scenario())


def test_image_and_audio_formats_have_deterministic_payloads(
    iris_board, firmware_profile
) -> None:
    assert firmware_profile == "services_usb"

    async def one(
        raw: RawIrisSession, channel: int, format_: int
    ) -> dict[str, Any]:
        assert raw.session is not None
        raw.media.clear()
        await raw.session.rpc(
            TEST_SERVICE_ID,
            MEDIA_CONFIGURE_METHOD,
            MEDIA_CONFIGURE_V1.pack(channel, format_, 10),
        )
        state = await raw.session.mirror_start(channel, fps=60)
        assert state["stream_id"] != 0
        try:
            return await raw.wait_media(channel)
        finally:
            await raw.session.mirror_stop(channel)

    async def scenario() -> None:
        raw = await _open_usb(iris_board)
        try:
            for format_, size in ((1, 128), (2, 192)):
                event = await one(raw, Channel.IMAGE, format_)
                assert event["description"]["format"] == format_
                assert len(event["data"]) == size
                assert event["frame_id"] > 0 and event["dropped"] >= 0
            jpeg = await one(raw, Channel.IMAGE, 3)
            assert jpeg["data"].startswith(b"\xff\xd8")
            assert jpeg["data"].endswith(b"\xff\xd9")
            png = await one(raw, Channel.IMAGE, 4)
            assert png["data"].startswith(b"\x89PNG\r\n\x1a\n")

            pcm = await one(raw, Channel.AUDIO, 0x100)
            assert pcm["description"] == {
                "x": 0,
                "y": 0,
                "width": 16000,
                "height": 1,
                "stride": 2,
                "format": 0x100,
                "quality": 0,
            }
            assert len(pcm["data"]) == 32
            opus = await one(raw, Channel.AUDIO, 0x101)
            assert opus["data"] == b"\xf8\xff\xfe"
        finally:
            await raw.close()

    run(scenario())


def test_fat_read_write_read_only_and_littlefs_atomic_replace(
    iris_board, firmware_profile
) -> None:
    assert firmware_profile == "services_usb"

    async def scenario() -> None:
        raw = await _open_usb(iris_board)
        try:
            assert raw.session is not None
            files = raw.session.files
            volumes = await files.volumes()
            by_id = {item["id"]: item for item in volumes["volumes"]}
            assert set(by_id) == {"fs", "ro", "atomic"}
            assert "write" in by_id["fs"]["capability_names"]
            assert "write" not in by_id["ro"]["capability_names"]
            assert "atomic_replace" in by_id["atomic"]["capability_names"]

            readme = b"".join(
                [chunk async for chunk in files.read_chunks("fs", "README.txt")]
            )
            assert b"hardware file-service fixture" in readme
            ranged = b"".join(
                [
                    chunk
                    async for chunk in files.read_chunks(
                        "fs", "README.txt", offset=4, length=4
                    )
                ]
            )
            assert ranged == readme[4:8]

            payload = b"alpha-" + b"beta-" + b"gamma"
            uploaded = await files.upload(
                "fs",
                "upload.bin",
                _chunks(b"alpha-", b"beta-", b"gamma"),
                total_size=len(payload),
            )
            assert uploaded["sha256"] == hashlib.sha256(payload).hexdigest()
            await files.mkdir("fs", "directory")
            await files.rename("fs", "upload.bin", "directory/renamed.bin")
            with pytest.raises(FileServiceError) as not_empty:
                await files.delete("fs", "directory")
            assert not_empty.value.status is FileStatus.NOT_EMPTY
            await files.delete("fs", "directory/renamed.bin")
            await files.delete("fs", "directory")

            with pytest.raises(FileServiceError) as read_only:
                await files.upload(
                    "ro", "denied.bin", _chunks(b"x"), total_size=1
                )
            assert read_only.value.status is FileStatus.READ_ONLY

            before = await files.stat("atomic", "current.txt")
            atomic_payload = b"atomically replaced\n"
            result = await files.upload(
                "atomic",
                "current.txt",
                _chunks(atomic_payload[:5], atomic_payload[5:]),
                total_size=len(atomic_payload),
                overwrite=True,
                if_match=before["etag"],
            )
            assert result["sha256"] == hashlib.sha256(atomic_payload).hexdigest()
            with pytest.raises(FileServiceError) as conflict:
                await files.upload(
                    "atomic",
                    "current.txt",
                    _chunks(b"stale"),
                    total_size=5,
                    overwrite=True,
                    if_match=before["etag"],
                )
            assert conflict.value.status is FileStatus.CONFLICT
        finally:
            await raw.close()

    run(scenario())


def test_raw_file_errors_abort_and_disconnect_cleanup(
    iris_board, firmware_profile
) -> None:
    assert firmware_profile == "services_usb"

    async def open_write(
        raw: RawIrisSession, path: str, total: int
    ) -> tuple[int, FileStatus]:
        frame = await raw.request(
            Channel.FILE,
            FileType.WRITE_OPEN,
            DeviceFiles._path_payload("fs", path)
            + struct.pack("<QQHH", total, 0, 0, 0),
            timeout=10,
        )
        status = _file_status(frame.payload)
        return (
            struct.unpack_from("<I", frame.payload, 4)[0]
            if status is FileStatus.OK
            else 0,
            status,
        )

    async def scenario() -> None:
        raw = await _open_usb(iris_board)
        try:
            stream, status = await open_write(raw, "bad-offset.bin", 3)
            assert status is FileStatus.OK
            frame = await raw.request(
                Channel.FILE,
                FileType.WRITE,
                struct.pack("<QHH", 1, 3, 0) + b"abc",
                stream_id=stream,
            )
            assert _file_status(frame.payload) is FileStatus.INVALID_ARGUMENT
            aborted = await raw.request(
                Channel.FILE, FileType.ABORT, stream_id=stream
            )
            assert _file_status(aborted.payload) is FileStatus.OK

            stream, _ = await open_write(raw, "bad-hash.bin", 3)
            written = await raw.request(
                Channel.FILE,
                FileType.WRITE,
                struct.pack("<QHH", 0, 3, 0) + b"abc",
                stream_id=stream,
            )
            assert _file_status(written.payload) is FileStatus.OK
            committed = await raw.request(
                Channel.FILE, FileType.COMMIT, b"\0" * 32, stream_id=stream
            )
            assert _file_status(committed.payload) is FileStatus.HASH_MISMATCH

            stream, _ = await open_write(raw, "busy.bin", 1)
            _, busy = await open_write(raw, "busy.bin", 1)
            assert busy is FileStatus.BUSY
            await raw.request(Channel.FILE, FileType.ABORT, stream_id=stream)

            _, no_space = await open_write(raw, "too-large.bin", 16 * 1024 * 1024)
            assert no_space is FileStatus.NO_SPACE

            volume = b"fs"
            traversal = b"../escape"
            frame = await raw.request(
                Channel.FILE,
                FileType.STAT_REQUEST,
                struct.pack("<BBH", len(volume), 0, len(traversal))
                + volume
                + traversal,
            )
            assert _file_status(frame.payload) is FileStatus.INVALID_ARGUMENT

            stream, _ = await open_write(raw, "disconnect.bin", 8)
            await raw.request(
                Channel.FILE,
                FileType.WRITE,
                struct.pack("<QHH", 0, 4, 0) + b"half",
                stream_id=stream,
            )
        finally:
            await raw.close()

        reconnected = await _open_usb(iris_board)
        try:
            assert reconnected.session is not None
            with pytest.raises(FileServiceError) as missing:
                await reconnected.session.files.stat("fs", "disconnect.bin")
            assert missing.value.status is FileStatus.NOT_FOUND
            listing = await reconnected.session.files.list_directory("fs", "")
            assert not any(".iris" in item["name"] for item in listing["entries"])
        finally:
            await reconnected.close()

    run(scenario())
