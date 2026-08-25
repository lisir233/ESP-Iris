import json
import zipfile

from iris_gateway.security import Actor, AuthManager, DEFAULT_DEVELOPER_PASSWORD
from iris_gateway.store import GatewayStore


def test_password_named_tokens_cursor_and_export(tmp_path) -> None:
    assert DEFAULT_DEVELOPER_PASSWORD == "espressif"
    store = GatewayStore(tmp_path)
    auth = AuthManager(store)
    auth.set_initial_password("dev-password")
    assert auth.verify_password("dev-password")
    assert not auth.verify_password("wrong-password")

    developer = Actor("developer", "Developer")
    created = auth.create_agent_token("codex-a", developer)
    assert created["token"].startswith("iris_")
    assert auth.authenticate_bearer(created["token"]) == Actor("agent", "codex-a")
    assert "token" not in auth.list_agent_tokens()[0]

    first = store.append_event("log", {"kind": "log", "text": "I (1) app: ok"}, "device-a")
    second = store.append_event("device", {"kind": "device_event", "event_name": "healthy"}, "device-a")
    events, gap = store.events_after(first["event_id"], device_id="device-a")
    assert not gap
    assert [item["event_id"] for item in events] == [second["event_id"]]

    for index in range(5):
        store.append_event("log", {"kind": "log", "text": f"line-{index}"}, "device-b")
    latest = store.latest_events(device_id="device-b", categories=["log"], limit=3)
    assert [item["text"] for item in latest] == ["line-2", "line-3", "line-4"]

    first_artifact = store.save_artifact("device-a", "screenshot", b"same", "png")
    second_artifact = store.save_artifact("device-a", "screenshot", b"same", "png")
    assert first_artifact != second_artifact
    assert first_artifact.read_bytes() == second_artifact.read_bytes() == b"same"

    store.create_operation(
        {
            "operation_id": "op-1",
            "device_id": "device-a",
            "actor_type": "agent",
            "actor_name": "codex-a",
            "action": "rpc.raw",
            "params": {"request_bytes": 1},
            "status": "succeeded",
            "created_ns": 1,
        }
    )
    archive = store.export_zip()
    with zipfile.ZipFile(archive) as bundle:
        names = set(bundle.namelist())
        assert {"manifest.json", "operations.jsonl", "system-audit.jsonl", "checksums.sha256"} <= names
        assert json.loads(bundle.read("manifest.json"))["schema"] == "esp-iris-export/v1"
    store.close()
