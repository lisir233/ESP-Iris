from __future__ import annotations

import asyncio
import ipaddress
import re
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass
from typing import Any

from zeroconf import IPVersion, ServiceStateChange
from zeroconf.asyncio import AsyncServiceBrowser, AsyncServiceInfo, AsyncZeroconf

IRIS_MDNS_TYPE = "_esp-iris._tcp.local."
_DEVICE_ID = re.compile(r"[0-9a-f]{32}\Z")


@dataclass(frozen=True)
class IrisMdnsDevice:
    service_name: str
    device_id: str
    host: str
    port: int
    mode: str
    pairing: str


def _decode_properties(properties: Mapping[Any, Any]) -> dict[str, str]:
    decoded: dict[str, str] = {}
    for raw_key, raw_value in properties.items():
        if raw_value is None:
            continue
        try:
            key = raw_key.decode("utf-8") if isinstance(raw_key, bytes) else str(raw_key)
            value = (
                raw_value.decode("utf-8")
                if isinstance(raw_value, bytes)
                else str(raw_value)
            )
        except UnicodeDecodeError:
            continue
        decoded[key] = value
    return decoded


def parse_iris_service(service_name: str, info: Any) -> IrisMdnsDevice | None:
    properties = _decode_properties(info.properties)
    device_id = properties.get("device_id", "")
    if (
        not _DEVICE_ID.fullmatch(device_id)
        or properties.get("protocol") != "1"
        or properties.get("transport") != "tcp"
        or properties.get("pairing") not in {"none", "hmac"}
        or not properties.get("mode")
    ):
        return None
    try:
        txt_port = int(properties.get("port", ""))
        srv_port = int(info.port)
    except (TypeError, ValueError):
        return None
    if not 1 <= srv_port <= 65535 or txt_port != srv_port:
        return None

    host = None
    for address in sorted(info.parsed_addresses(IPVersion.V4Only)):
        try:
            if isinstance(ipaddress.ip_address(address), ipaddress.IPv4Address):
                host = address
                break
        except ValueError:
            continue
    if host is None:
        scoped = getattr(info, "parsed_scoped_addresses", None)
        ipv6 = (
            scoped(IPVersion.V6Only)
            if scoped is not None
            else info.parsed_addresses(IPVersion.V6Only)
        )
        for address in sorted(ipv6):
            raw_address = address.split("%", 1)[0]
            try:
                parsed = ipaddress.ip_address(raw_address)
            except ValueError:
                continue
            if isinstance(parsed, ipaddress.IPv6Address) and (
                not parsed.is_link_local or "%" in address
            ):
                host = address
                break
    if host is None:
        return None
    return IrisMdnsDevice(
        service_name=service_name,
        device_id=device_id,
        host=host,
        port=srv_port,
        mode=properties["mode"],
        pairing=properties["pairing"],
    )


class IrisMdnsDiscovery:
    def __init__(
        self,
        on_service: Callable[[IrisMdnsDevice], Awaitable[None]],
        on_remove: Callable[[str], Awaitable[None]],
    ) -> None:
        self._on_service = on_service
        self._on_remove = on_remove
        self._zeroconf: AsyncZeroconf | None = None
        self._browser: AsyncServiceBrowser | None = None
        self._tasks: set[asyncio.Task[None]] = set()

    async def start(self) -> None:
        if self._zeroconf is not None:
            return
        self._zeroconf = AsyncZeroconf(ip_version=IPVersion.All)
        self._browser = AsyncServiceBrowser(
            self._zeroconf.zeroconf,
            IRIS_MDNS_TYPE,
            handlers=[self._on_state_change],
        )

    def _on_state_change(
        self,
        zeroconf: Any,
        service_type: str,
        name: str,
        state_change: ServiceStateChange,
    ) -> None:
        coroutine = (
            self._remove(name)
            if state_change is ServiceStateChange.Removed
            else self._resolve(zeroconf, service_type, name)
        )
        task: asyncio.Task[None] = asyncio.create_task(coroutine)
        self._tasks.add(task)
        task.add_done_callback(self._task_done)

    def _task_done(self, task: asyncio.Task[None]) -> None:
        self._tasks.discard(task)
        if not task.cancelled():
            task.exception()

    async def _remove(self, name: str) -> None:
        await self._on_remove(name)

    async def _resolve(self, zeroconf: Any, service_type: str, name: str) -> None:
        info = AsyncServiceInfo(service_type, name)
        if not await info.async_request(zeroconf, 3000):
            return
        device = parse_iris_service(name, info)
        if device is not None:
            await self._on_service(device)

    async def close(self) -> None:
        if self._browser is not None:
            await self._browser.async_cancel()
            self._browser = None
        tasks = tuple(self._tasks)
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        self._tasks.clear()
        if self._zeroconf is not None:
            await self._zeroconf.async_close()
            self._zeroconf = None
