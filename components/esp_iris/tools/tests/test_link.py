import asyncio
import threading
import time

from iris_gateway.link import SerialLink


class FakeSerial:
    def __init__(self, reads: list[bytes]) -> None:
        self.reads = reads
        self.buffered = b""
        self.is_open = True
        self.calls = 0

    @property
    def in_waiting(self) -> int:
        return len(self.buffered)

    def read(self, size: int) -> bytes:
        self.calls += 1
        if not self.buffered:
            self.buffered = self.reads.pop(0)
        result = self.buffered[:size]
        self.buffered = self.buffered[size:]
        return result

    def close(self) -> None:
        self.is_open = False

    def write(self, data: bytes) -> int:
        return len(data)


def test_serial_timeout_is_not_treated_as_disconnect() -> None:
    async def scenario() -> None:
        serial = FakeSerial([b"", b"", b"frame\x00"])
        link = SerialLink("/dev/fake-iris", serial)
        assert await asyncio.wait_for(link.read(), 1.0) == b"frame\x00"
        assert serial.calls == 4
        await link.close()

    asyncio.run(scenario())


def test_serial_read_batches_already_buffered_frames() -> None:
    async def scenario() -> None:
        serial = FakeSerial([b"first\x00second\x00third\x00"])
        link = SerialLink("/dev/fake-iris", serial)
        assert await link.read() == b"first\x00second\x00third\x00"
        await link.close()

    asyncio.run(scenario())


def test_serial_write_is_not_blocked_by_an_in_flight_read() -> None:
    class DuplexSerial(FakeSerial):
        def __init__(self) -> None:
            super().__init__([])
            self.read_started = threading.Event()
            self.release_read = threading.Event()
            self.writes: list[bytes] = []

        def read(self, size: int) -> bytes:
            self.read_started.set()
            self.release_read.wait(0.5)
            return b"frame"

        def write(self, data: bytes) -> int:
            self.writes.append(data)
            return len(data)

    async def scenario() -> None:
        serial = DuplexSerial()
        link = SerialLink("/dev/fake-iris", serial)
        read_task = asyncio.create_task(link.read())
        assert await asyncio.to_thread(serial.read_started.wait, 0.5)
        await asyncio.wait_for(link.write(b"request"), 0.1)
        assert serial.writes == [b"request"]
        serial.release_read.set()
        assert await asyncio.wait_for(read_task, 1.0) == b"frame"
        await link.close()

    asyncio.run(scenario())


def test_serial_read_returns_eof_after_close() -> None:
    async def scenario() -> None:
        serial = FakeSerial([])
        serial.is_open = False
        link = SerialLink("/dev/fake-iris", serial)
        assert await link.read() == b""

    asyncio.run(scenario())


def test_serial_close_waits_for_in_flight_read() -> None:
    class SlowSerial(FakeSerial):
        def read(self, size: int) -> bytes:
            time.sleep(0.02)
            return b""

    async def scenario() -> None:
        serial = SlowSerial([])
        link = SerialLink("/dev/fake-iris", serial)
        read_task = asyncio.create_task(link.read())
        await asyncio.sleep(0)
        await link.close()
        assert await asyncio.wait_for(read_task, 1.0) == b""

    asyncio.run(scenario())


def test_serial_cancel_drains_in_flight_thread_before_close() -> None:
    class SlowSerial(FakeSerial):
        def read(self, size: int) -> bytes:
            time.sleep(0.02)
            return b""

    async def scenario() -> None:
        serial = SlowSerial([])
        link = SerialLink("/dev/fake-iris", serial)
        read_task = asyncio.create_task(link.read())
        await asyncio.sleep(0)
        read_task.cancel()
        try:
            await read_task
        except asyncio.CancelledError:
            pass
        await link.close()
        assert serial.is_open is False

    asyncio.run(scenario())
