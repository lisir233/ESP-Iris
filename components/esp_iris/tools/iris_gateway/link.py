from __future__ import annotations

import abc
import asyncio
import contextlib
import hashlib
import os
import pathlib
import tempfile
from typing import BinaryIO


class Link(abc.ABC):
    endpoint: str

    @abc.abstractmethod
    async def read(self, size: int = 4096) -> bytes: ...

    @abc.abstractmethod
    async def write(self, data: bytes) -> None: ...

    @abc.abstractmethod
    async def close(self) -> None: ...


class TcpLink(Link):
    def __init__(
        self,
        host: str,
        port: int,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        self.host = host
        self.port = port
        self.endpoint = f"tcp:{host}:{port}"
        self._reader = reader
        self._writer = writer

    @classmethod
    async def open(cls, host: str, port: int) -> TcpLink:
        reader, writer = await asyncio.open_connection(host, port)
        return cls(host, port, reader, writer)

    async def read(self, size: int = 4096) -> bytes:
        return await self._reader.read(size)

    async def write(self, data: bytes) -> None:
        self._writer.write(data)
        await self._writer.drain()

    async def close(self) -> None:
        self._writer.close()
        await self._writer.wait_closed()


class SerialLink(Link):
    def __init__(self, port: str, serial_port: BinaryIO) -> None:
        self.port = port
        self.endpoint = f"usb:{os.path.realpath(port)}"
        self._serial = serial_port
        self._read_lock = asyncio.Lock()
        self._write_lock = asyncio.Lock()
        self._close_lock = asyncio.Lock()
        self._closing = False

    @classmethod
    async def open(
        cls,
        port: str,
        *,
        hupcl: bool | None = None,
    ) -> SerialLink:
        import serial

        def open_port() -> BinaryIO:
            options = {
                "baudrate": 115200,
                "timeout": 0.2,
                "write_timeout": 2,
                "exclusive": True if os.name == "posix" else None,
            }
            serial_port = serial.Serial(port=port, **options)
            try:
                if hupcl is not None and os.name == "posix":
                    import termios

                    attributes = termios.tcgetattr(serial_port.fileno())
                    if hupcl:
                        attributes[2] |= termios.HUPCL
                    else:
                        attributes[2] &= ~termios.HUPCL
                    termios.tcsetattr(
                        serial_port.fileno(), termios.TCSANOW, attributes
                    )
            except BaseException:
                serial_port.close()
                raise
            return serial_port

        serial_port = await asyncio.to_thread(open_port)
        return cls(port, serial_port)

    def _read_batch(self, size: int) -> bytes:
        if size <= 0:
            return b""
        data = self._serial.read(1)
        if not data or size == 1:
            return data
        waiting = int(getattr(self._serial, "in_waiting", 0) or 0)
        if waiting <= 0:
            return data
        return data + self._serial.read(min(waiting, size - len(data)))

    async def read(self, size: int = 65536) -> bytes:
        while True:
            async with self._read_lock:
                if self._closing or not self._serial.is_open:
                    return b""
                read_task = asyncio.create_task(
                    asyncio.to_thread(self._read_batch, size)
                )
                try:
                    data = await asyncio.shield(read_task)
                except asyncio.CancelledError:
                    cancel_read = getattr(self._serial, "cancel_read", None)
                    if cancel_read is not None:
                        await asyncio.to_thread(cancel_read)
                    with contextlib.suppress(Exception):
                        await read_task
                    raise
            if data:
                return data
            if self._closing:
                return b""
            await asyncio.sleep(0)

    async def write(self, data: bytes) -> None:
        async with self._write_lock:
            if self._closing or not self._serial.is_open:
                raise ConnectionError("ESP-Iris serial link is closed")
            written = await asyncio.to_thread(self._serial.write, data)
        if written != len(data):
            raise OSError(f"short serial write: {written}/{len(data)}")

    async def close(self) -> None:
        async with self._close_lock:
            if not self._serial.is_open:
                return
            self._closing = True
            cancel_read = getattr(self._serial, "cancel_read", None)
            if cancel_read is not None:
                with contextlib.suppress(Exception):
                    await asyncio.to_thread(cancel_read)
            async with self._read_lock, self._write_lock:
                if self._serial.is_open:
                    await asyncio.to_thread(self._serial.close)


class EndpointLock:
    """Cross-process advisory lock for one physical endpoint."""

    def __init__(self, endpoint: str) -> None:
        digest = hashlib.sha256(endpoint.encode()).hexdigest()
        root = pathlib.Path(tempfile.gettempdir()) / "esp-iris-locks"
        root.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.path = root / f"{digest}.lock"
        self._file = self.path.open("a+b")
        self._file.seek(0, os.SEEK_END)
        if self._file.tell() == 0:
            # msvcrt.locking() needs a real byte range on first use.
            self._file.write(b"\0")
            self._file.flush()

    def acquire(self) -> None:
        if os.name == "nt":
            import msvcrt

            self._file.seek(0)
            try:
                msvcrt.locking(self._file.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as exc:
                raise RuntimeError(
                    "endpoint is owned by another ESP-Iris instance"
                ) from exc
        else:
            import fcntl

            try:
                fcntl.flock(self._file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as exc:
                raise RuntimeError(
                    "endpoint is owned by another ESP-Iris instance"
                ) from exc
        self._file.seek(0)
        self._file.truncate()
        self._file.write(f"pid={os.getpid()}\nendpoint={self.path.name}\n".encode())
        self._file.flush()

    def close(self) -> None:
        if self._file.closed:
            return
        if os.name == "nt":
            import msvcrt

            self._file.seek(0)
            try:
                msvcrt.locking(self._file.fileno(), msvcrt.LK_UNLCK, 1)
            except OSError:
                pass
        else:
            import fcntl

            fcntl.flock(self._file.fileno(), fcntl.LOCK_UN)
        self._file.close()
