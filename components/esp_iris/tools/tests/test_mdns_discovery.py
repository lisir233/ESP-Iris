import asyncio
from types import SimpleNamespace

import pytest
from zeroconf import IPVersion

from iris_gateway.hub import IrisHub
from iris_gateway.mdns_discovery import IrisMdnsDevice, parse_iris_service


class FakeServiceInfo:
    def __init__(
        self,
        *,
        properties: dict[bytes, bytes] | None = None,
        port: int = 19772,
        ipv4: list[str] | None = None,
        ipv6: list[str] | None = None,
    ) -> None:
        self.properties = properties or {
            b"device_id": b"00112233445566778899aabbccddeeff",
            b"protocol": b"1",
            b"transport": b"tcp",
            b"pairing": b"none",
            b"mode": b"normal",
            b"port": b"19772",
        }
        self.port = port
        self.ipv4 = ipv4 or []
        self.ipv6 = ipv6 or []

    def parsed_addresses(self, version: IPVersion) -> list[str]:
        return self.ipv4 if version is IPVersion.V4Only else self.ipv6

    def parsed_scoped_addresses(self, version: IPVersion) -> list[str]:
        return self.parsed_addresses(version)


def test_mdns_service_parsing_prefers_ipv4() -> None:
    device = parse_iris_service(
        "ESP-Iris-abcdef._esp-iris._tcp.local.",
        FakeServiceInfo(ipv4=["192.0.2.8"], ipv6=["2001:db8::8"]),
    )
    assert device is not None
    assert device.host == "192.0.2.8"
    assert device.device_id == "00112233445566778899aabbccddeeff"


@pytest.mark.parametrize(
    ("key", "value"),
    [
        (b"device_id", b"not-a-device-id"),
        (b"protocol", b"2"),
        (b"transport", b"udp"),
        (b"pairing", b"token-in-txt"),
        (b"port", b"19773"),
    ],
)
def test_mdns_service_rejects_invalid_contract(key: bytes, value: bytes) -> None:
    info = FakeServiceInfo(ipv4=["192.0.2.8"])
    info.properties[key] = value
    assert parse_iris_service("invalid._esp-iris._tcp.local.", info) is None


def test_mdns_service_accepts_scoped_link_local_ipv6_only() -> None:
    assert (
        parse_iris_service(
            "unscoped._esp-iris._tcp.local.",
            FakeServiceInfo(ipv6=["fe80::1234"]),
        )
        is None
    )
    device = parse_iris_service(
        "scoped._esp-iris._tcp.local.",
        FakeServiceInfo(ipv6=["fe80::1234%eth0"]),
    )
    assert device is not None
    assert device.host == "fe80::1234%eth0"


def test_hub_tracks_mdns_updates_and_preserves_manual_endpoints() -> None:
    async def scenario() -> None:
        hub = IrisHub("mdns-test")
        calls: list[tuple[str, int, str | None, dict[str, object]]] = []

        async def fake_add_tcp(
            host: str,
            port: int = 19772,
            pairing_token: str | None = None,
            *,
            metadata: dict[str, object] | None = None,
        ) -> None:
            endpoint = f"tcp:{host}:{port}"
            calls.append((host, port, pairing_token, metadata or {}))
            hub._endpoint_states[endpoint] = {
                "endpoint": endpoint,
                **(metadata or {}),
            }
            hub._endpoint_tasks[endpoint] = asyncio.create_task(
                asyncio.Event().wait()
            )

        hub.add_tcp = fake_add_tcp  # type: ignore[method-assign]
        service = "ESP-Iris-aabbcc._esp-iris._tcp.local."
        first = IrisMdnsDevice(
            service, "00112233445566778899aabbccddeeff", "192.0.2.8", 19772,
            "normal", "hmac"
        )
        await hub._add_mdns_device(first, "pairing-secret")
        assert calls[0][2] == "pairing-secret"
        assert hub.list_endpoints()[0]["discovery"] == "mdns"

        moved = IrisMdnsDevice(
            service, first.device_id, "192.0.2.9", 19772, "recovery", "hmac"
        )
        await hub._add_mdns_device(moved, "pairing-secret")
        assert [item[0] for item in calls] == ["192.0.2.8", "192.0.2.9"]
        assert hub.list_endpoints()[0]["firmware_mode"] == "recovery"
        await hub._remove_mdns_service(service)
        assert hub.list_endpoints() == []

        manual_endpoint = "tcp:192.0.2.10:19772"
        hub._endpoint_states[manual_endpoint] = {"endpoint": manual_endpoint}
        hub._endpoint_tasks[manual_endpoint] = asyncio.create_task(
            asyncio.Event().wait()
        )
        manual = IrisMdnsDevice(
            service, first.device_id, "192.0.2.10", 19772, "normal", "none"
        )
        await hub._add_mdns_device(manual, None)
        await hub._remove_mdns_service(service)
        assert manual_endpoint in hub._endpoint_tasks
        await hub.close()

    asyncio.run(scenario())


def test_hub_rejects_mdns_identity_mismatch() -> None:
    async def scenario() -> None:
        hub = IrisHub("mdns-identity-test")
        endpoint = "tcp:192.0.2.8:19772"
        hub._endpoint_states[endpoint] = {
            "advertised_device_id": "00112233445566778899aabbccddeeff"
        }
        closed = False

        async def close() -> None:
            nonlocal closed
            closed = True

        session = SimpleNamespace(
            info=SimpleNamespace(device_id="ffeeddccbbaa99887766554433221100"),
            link=SimpleNamespace(endpoint=endpoint),
            close=close,
        )
        with pytest.raises(RuntimeError, match="mDNS device_id"):
            await hub._on_ready(session)
        assert closed is True

    asyncio.run(scenario())
