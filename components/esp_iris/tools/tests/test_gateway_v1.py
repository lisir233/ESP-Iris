import asyncio
import hashlib
import json
import struct
from unittest.mock import Mock

from aiohttp import FormData
from aiohttp.test_utils import make_mocked_request
from aiohttp.test_utils import TestClient, TestServer
from iris_gateway.cli import _client_ssl, _listen_is_loopback, build_parser
from iris_gateway.demo import DemoHub
from iris_gateway.gateway import GatewayService, create_app
from iris_gateway.security import Actor
from iris_gateway.store import GatewayStore


def _firmware_bundle() -> tuple[bytes, bytes, bytes]:
    elf = b"test-application-elf"
    image = bytearray(256)
    image[0] = 0xE9
    image[1] = 1
    struct.pack_into("<H", image, 12, 0x20)
    descriptor = 32
    struct.pack_into("<I", image, descriptor, 0xABCD5432)
    image[descriptor + 16 : descriptor + 21] = b"5.0.0"
    image[descriptor + 48 : descriptor + 65] = b"esp-iris-template"
    image[descriptor + 144 : descriptor + 176] = hashlib.sha256(elf).digest()
    return bytes(image), elf, b"memory map"


def test_ota_archives_complete_bundle_and_returns_queryable_operation(tmp_path) -> None:
    async def scenario() -> None:
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test", demo=True)
        hub = DemoHub(service.on_device_event)
        service.attach_hub(hub)
        await hub.start()
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            binary, elf, map_data = _firmware_bundle()
            form = FormData()
            form.add_field("bin", binary, filename="app.bin")
            form.add_field("elf", elf, filename="app.elf")
            form.add_field("map", map_data, filename="app.map")
            uploaded = await client.post("/v1/firmware-artifacts", data=form)
            assert uploaded.status == 201
            artifact = (await uploaded.json())["artifact"]
            assert artifact["artifact_id"] == hashlib.sha256(elf).hexdigest()

            accepted = await client.post(
                "/v1/devices/demo-a1b2c3d4/ota",
                json={
                    "artifact_id": artifact["artifact_id"],
                    "execution_mode": "recovery",
                },
                headers={"X-Operation-ID": "ota-background"},
            )
            assert accepted.status == 202
            assert (await accepted.json())["status_url"].endswith("ota-background")
            operation = None
            for _ in range(100):
                response = await client.get("/v1/operations/ota-background")
                operation = await response.json()
                if operation["status"] == "succeeded":
                    break
                await asyncio.sleep(0.01)
            assert operation is not None
            assert operation["status"] == "succeeded"
            assert operation["result"]["execution_mode"] == "recovery"
            assert "preserved_coredump" not in operation["result"]
        finally:
            await client.close()
            await hub.close()
            store.close()

    asyncio.run(scenario())


def test_authenticated_gateway_mode_and_idempotent_operations(tmp_path) -> None:
    async def scenario() -> None:
        store = GatewayStore(tmp_path)
        service = GatewayService(
            store, instance_id="test", demo=True, require_local_auth=True
        )
        service.auth.set_initial_password("dev-password")
        hub = DemoHub(service.on_device_event)
        service.attach_hub(hub)
        await hub.start()
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            unauthorized = await client.get("/v1/devices")
            assert unauthorized.status == 401
            login = await client.post("/v1/auth/login", json={"password": "dev-password"})
            assert login.status == 200
            session = login.cookies["esp_iris_session"].value
            headers = {"Cookie": f"esp_iris_session={session}"}

            devices = await client.get("/v1/devices", headers=headers)
            body = await devices.json()
            assert len(body["devices"]) == 3
            assert body["demo"] is True

            status = await client.get("/v1/devices/demo-a1b2c3d4", headers=headers)
            assert (await status.json())["stale"] is False

            console = await client.post(
                "/v1/devices/demo-a1b2c3d4/console",
                json={"line": "help"},
                headers=headers,
            )
            console_body = await console.json()
            assert console.status == 200
            assert console_body["console"] == {"job_id": 1, "accepted": True}
            assert console_body["operation"]["params"]["command"] == "help"
            assert "line" not in console_body["operation"]["params"]

            invalid_console = await client.post(
                "/v1/devices/demo-a1b2c3d4/console",
                json={"line": "help\nrestart"},
                headers=headers,
            )
            assert invalid_console.status == 400

            operation_headers = {**headers, "X-Operation-ID": "stable-op"}
            payload = {"service_id": 1, "method_id": 1, "payload_text": "hi"}
            first = await client.post(
                "/v1/devices/demo-a1b2c3d4/rpc/raw",
                json=payload,
                headers=operation_headers,
            )
            assert (await first.json())["operation"]["status"] == "succeeded"
            second = await client.post(
                "/v1/devices/demo-a1b2c3d4/rpc/raw",
                json=payload,
                headers=operation_headers,
            )
            assert (await second.json())["idempotent_reuse"] is True

            screenshot = await client.post(
                "/v1/devices/demo-a1b2c3d4/screenshot",
                json={"width": 160, "height": 90},
                headers=headers,
            )
            assert screenshot.content_type == "image/png"
            assert (await screenshot.read()).startswith(b"\x89PNG")

            mirror = await client.post(
                "/v1/devices/demo-a1b2c3d4/mirror/start",
                json={"channel": "screen", "fps": 5, "description": {}},
                headers=headers,
            )
            assert mirror.status == 200
            repeated_mirror = await client.post(
                "/v1/devices/demo-a1b2c3d4/mirror/start",
                json={"channel": "screen", "fps": 5, "description": {}},
                headers=headers,
            )
            assert repeated_mirror.status == 200
            assert (await repeated_mirror.json())["mirror"]["reused"] is True
            mirrored_screenshot = await client.post(
                "/v1/devices/demo-a1b2c3d4/screenshot",
                json={},
                headers=headers,
            )
            media = json.loads(mirrored_screenshot.headers["X-ESP-Iris-Media"])
            assert media["mirror_reused"] == 1
            stopped = await client.post(
                "/v1/devices/demo-a1b2c3d4/mirror/stop",
                json={"channel": "screen"},
                headers=headers,
            )
            assert stopped.status == 200

            gesture = await client.post(
                "/v1/devices/demo-a1b2c3d4/input",
                json={
                    "begin": {"x": 0, "y": 0},
                    "moves": [{"x": 5000, "y": 5000}],
                    "end": {"x": 10000, "y": 10000},
                },
                headers=headers,
            )
            assert (await gesture.json())["input"]["points"] == 3

            restart = await client.post(
                "/v1/devices/demo-a1b2c3d4/restart",
                json={"delay_ms": 250},
                headers=headers,
            )
            assert (await restart.json())["restart"]["reconnected"] is True

            factory = await client.post(
                "/v1/devices/demo-a1b2c3d4/factory-recovery",
                headers=headers,
            )
            assert (await factory.json())["recovery"]["target"] == "factory_recovery"

            observe = await client.put("/v1/mode", json={"mode": "observe"}, headers=headers)
            assert (await observe.json())["mode"] == "observe"
            cached = await client.get("/v1/devices/demo-a1b2c3d4", headers=headers)
            assert (await cached.json())["stale"] is True
            blocked = await client.post(
                "/v1/devices/demo-a1b2c3d4/restart",
                json={"delay_ms": 250},
                headers=headers,
            )
            assert blocked.status == 423
            assert (await blocked.json())["error"]["code"] == "observe_mode"

            spec = await client.get("/v1/openapi.json", headers=headers)
            spec_body = await spec.json()
            assert spec_body["openapi"] == "3.1.0"
            assert "security" in spec_body
        finally:
            await client.close()
            await hub.close()
            store.close()

    asyncio.run(scenario())


def test_named_agent_token_can_switch_mode_without_approval(tmp_path) -> None:
    async def scenario() -> None:
        store = GatewayStore(tmp_path)
        service = GatewayService(
            store, instance_id="test", demo=True, require_local_auth=True
        )
        service.auth.set_initial_password("dev-password")
        agent = service.auth.create_agent_token(
            "codex-bench", Actor("developer", "Developer")
        )
        hub = DemoHub(service.on_device_event)
        service.attach_hub(hub)
        await hub.start()
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            headers = {"Authorization": f"Bearer {agent['token']}"}
            response = await client.put("/v1/mode", json={"mode": "observe"}, headers=headers)
            assert response.status == 200
            audit = service.store.audits()[0]
            assert audit["actor_type"] == "agent"
            assert audit["actor_name"] == "codex-bench"
        finally:
            await client.close()
            await hub.close()
            store.close()

    asyncio.run(scenario())


def test_gateway_restart_always_defaults_to_develop(tmp_path) -> None:
    store = GatewayStore(tmp_path)
    store.set_setting("mode", "observe")
    service = GatewayService(store, instance_id="test")
    assert service.mode == "develop"
    assert store.get_setting("mode") == "develop"
    store.close()


def test_event_socket_sends_recent_history_then_live_logs(tmp_path) -> None:
    async def scenario() -> None:
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test")
        store.append_event(
            "log", {"kind": "log", "text": "I (1) app: stored\n"}, "device-a"
        )
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            websocket = await client.ws_connect(
                "/v1/events/ws?cursor=0&device_id=device-a"
            )
            assert (await websocket.receive_json())["text"].endswith("stored\n")
            await service.on_device_event(
                {"kind": "log", "device_id": "device-a", "text": "I (2) app: live\n"}
            )
            assert (await websocket.receive_json())["text"].endswith("live\n")
            await websocket.close()
        finally:
            await client.close()
            store.close()

    asyncio.run(scenario())


def test_frontend_entry_revalidates_while_hashed_assets_are_immutable(tmp_path) -> None:
    async def scenario() -> None:
        store = GatewayStore(tmp_path / "state")
        frontend = tmp_path / "frontend"
        assets = frontend / "assets"
        assets.mkdir(parents=True)
        (frontend / "index.html").write_text("<main>current workbench</main>")
        (assets / "index-current123.js").write_text("console.log('current')")
        service = GatewayService(store, instance_id="test", frontend_dist=frontend)
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            entry = await client.get("/")
            assert entry.status == 200
            assert entry.headers["Cache-Control"] == "no-store, no-cache, must-revalidate"
            assert entry.headers["Pragma"] == "no-cache"

            fallback = await client.get("/workspace")
            assert fallback.status == 200
            assert fallback.headers["Cache-Control"] == "no-store, no-cache, must-revalidate"

            asset = await client.get("/assets/index-current123.js")
            assert asset.status == 200
            assert asset.headers["Cache-Control"] == "public, max-age=31536000, immutable"
        finally:
            await client.close()
            store.close()

    asyncio.run(scenario())


def test_loopback_is_unauthenticated_by_default_and_forwarded_header_is_ignored(
    tmp_path,
) -> None:
    async def scenario() -> None:
        store = GatewayStore(tmp_path)
        service = GatewayService(store, instance_id="test", demo=True)
        hub = DemoHub(service.on_device_event)
        service.attach_hub(hub)
        await hub.start()
        client = TestClient(TestServer(create_app(service)))
        await client.start_server()
        try:
            health = await client.get("/v1/health")
            health_body = await health.json()
            assert health_body["authenticated_api"] is False
            assert health_body["local_authentication_required"] is False

            state = await client.get(
                "/v1/auth/state", headers={"X-Forwarded-For": "203.0.113.7"}
            )
            state_body = await state.json()
            assert state_body["required"] is False
            assert state_body["authenticated"] is True
            assert state_body["actor"]["type"] == "local"

            spec = await client.get("/v1/openapi.json")
            assert "security" not in await spec.json()

            devices = await client.get(
                "/v1/devices", headers={"X-Forwarded-For": "203.0.113.7"}
            )
            assert devices.status == 200
            assert len((await devices.json())["devices"]) == 3
        finally:
            await client.close()
            await hub.close()
            store.close()

    asyncio.run(scenario())


def test_non_loopback_peer_requires_authentication_by_default(tmp_path) -> None:
    store = GatewayStore(tmp_path)
    service = GatewayService(store, instance_id="test")
    transport = Mock()
    transport.get_extra_info.return_value = ("192.0.2.10", 42000)
    request = make_mocked_request("GET", "/v1/devices", transport=transport)
    assert service.authentication_required(request) is True
    store.close()


def test_local_auth_cli_is_opt_in() -> None:
    parser = build_parser()
    default_web = parser.parse_args(["web"])
    assert default_web.require_local_auth is False
    assert default_web.tls is False
    assert default_web.no_tls is False
    assert parser.parse_args(["web", "--require-local-auth"]).require_local_auth is True
    assert parser.parse_args(["web", "--tls"]).tls is True
    assert _client_ssl({"url": "http://192.0.2.10:8443"}, insecure=False) is False
    assert _listen_is_loopback("127.0.0.2") is True
    assert _listen_is_loopback("::ffff:127.0.0.1") is True
    assert _listen_is_loopback("0.0.0.0") is False
    screenshot = parser.parse_args(
        ["ctl", "--json", "screenshot", "device-a", "/tmp/device.png"]
    )
    assert screenshot.ctl_command == "screenshot"
    assert screenshot.output == "/tmp/device.png"
    crash = parser.parse_args(["ctl", "--json", "crash", "device-a"])
    assert crash.ctl_command == "crash"
    assert crash.device == "device-a"
    coredump = parser.parse_args(
        ["ctl", "--json", "coredump", "device-a", "/tmp/device.core"]
    )
    assert coredump.ctl_command == "coredump"
    assert coredump.output == "/tmp/device.core"
    pointer_input = parser.parse_args(
        [
            "ctl",
            "--json",
            "input",
            "device-a",
            "--gesture",
            '{"begin":{"x":1,"y":2},"moves":[],"end":{"x":1,"y":2}}',
        ]
    )
    assert pointer_input.ctl_command == "input"
    assert pointer_input.device == "device-a"
    mirror = parser.parse_args(
        ["ctl", "--json", "mirror", "device-a", "start", "--fps", "7"]
    )
    assert mirror.ctl_command == "mirror"
    assert mirror.action == "start"
    assert mirror.fps == 7
    console = parser.parse_args(
        ["ctl", "--json", "console", "device-a", "iris_info"]
    )
    assert console.ctl_command == "console"
    assert console.line == ["iris_info"]
