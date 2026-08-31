from __future__ import annotations

import asyncio
import contextlib
from collections.abc import AsyncIterator
from typing import Any, Self

from iris_gateway.link import SerialLink, TcpLink
from iris_gateway.protocol import Frame, ProtocolError, encode_frame
from iris_gateway.session import DeviceInfo, DeviceSession


class RawIrisSession:
    """Direct device session for frames the public Gateway never emits."""

    def __init__(
        self,
        port: str,
        *,
        usb_serial_jtag: bool = False,
        tcp_host: str | None = None,
        pairing_token: str | None = None,
    ) -> None:
        self.port = port
        self.usb_serial_jtag = usb_serial_jtag
        self.tcp_host = tcp_host
        self.pairing_token = pairing_token
        self.events: list[dict[str, Any]] = []
        self.media: list[dict[str, Any]] = []
        self.session: DeviceSession | None = None
        self.task: asyncio.Task[None] | None = None

    async def open(self) -> DeviceInfo:
        if self.tcp_host is None:
            link = await SerialLink.open(
                self.port, hupcl=False if self.usb_serial_jtag else None
            )
        else:
            link = await TcpLink.open(self.tcp_host, int(self.port))

        async def ready(session: DeviceSession) -> None:
            del session

        async def event(value: dict[str, Any]) -> None:
            self.events.append(value)

        async def media(value: dict[str, Any]) -> None:
            self.media.append(value)

        self.session = DeviceSession(
            link,
            ready,
            event,
            on_media=media,
            pairing_token=self.pairing_token,
        )
        self.task = asyncio.create_task(self.session.run())
        ready_task = asyncio.create_task(self.session.wait_ready(10))
        try:
            done, _ = await asyncio.wait(
                {ready_task, self.task},
                timeout=10,
                return_when=asyncio.FIRST_COMPLETED,
            )
        except asyncio.CancelledError:
            ready_task.cancel()
            await self.close()
            raise
        if self.task in done:
            ready_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await ready_task
            await self.task
            raise ConnectionError("Iris session closed before HELLO completed")
        if ready_task not in done:
            ready_task.cancel()
            await self.close()
            raise TimeoutError("Iris HELLO did not complete")
        return await ready_task

    async def close(self) -> None:
        if self.session is not None:
            await self.session.close()
        if self.task is not None:
            self.task.cancel()
            with contextlib.suppress(
                asyncio.CancelledError,
                ConnectionError,
                OSError,
                ProtocolError,
                RuntimeError,
            ):
                await self.task

    async def request(
        self,
        channel: int,
        type_: int,
        payload: bytes = b"",
        *,
        stream_id: int = 0,
        timeout: float = 3,
    ) -> Frame:
        assert self.session is not None
        return await self.session._request(
            channel, type_, payload, timeout, stream_id=stream_id
        )

    async def send_frame(self, frame: Frame) -> None:
        assert self.session is not None
        await self.session.link.write(encode_frame(frame))

    async def send_wire(self, wire: bytes) -> None:
        assert self.session is not None
        await self.session.link.write(wire)

    async def wait_media(self, channel: int, timeout: float = 5) -> dict[str, Any]:
        deadline = asyncio.get_running_loop().time() + timeout
        while asyncio.get_running_loop().time() < deadline:
            for event in self.media:
                if event.get("channel") == channel:
                    return event
            await asyncio.sleep(0.01)
        raise TimeoutError(f"no media frame arrived on channel {channel}")

    async def __aenter__(self) -> Self:
        await self.open()
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        await self.close()


@contextlib.asynccontextmanager
async def raw_session(
    port: str, *, usb_serial_jtag: bool = False
) -> AsyncIterator[RawIrisSession]:
    session = RawIrisSession(port, usb_serial_jtag=usb_serial_jtag)
    await session.open()
    try:
        yield session
    finally:
        await session.close()
