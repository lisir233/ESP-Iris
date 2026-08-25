import asyncio
import hashlib

from iris_gateway.operations import OperationManager
from iris_gateway.security import Actor
from iris_gateway.store import GatewayStore


def test_complete_firmware_artifact_is_content_addressed(tmp_path) -> None:
    store = GatewayStore(tmp_path)
    binary = b"application-bin"
    elf = b"application-elf"
    map_data = b"application-map"
    metadata = {
        "project_name": "alien_ship_game",
        "version": "5.0.0",
        "chip_id": 0x20,
    }
    artifact = store.save_firmware_artifact(
        binary=binary, elf=elf, map_data=map_data, metadata=metadata
    )
    assert artifact["artifact_id"] == hashlib.sha256(elf).hexdigest()
    assert artifact["binary_sha256"] == hashlib.sha256(binary).hexdigest()
    assert artifact["sizes"] == {
        "firmware.bin": len(binary),
        "firmware.elf": len(elf),
        "firmware.map": len(map_data),
    }
    assert store.save_firmware_artifact(
        binary=binary, elf=elf, map_data=map_data, metadata=metadata
    )["artifact_id"] == artifact["artifact_id"]
    store.close()


def test_background_operation_exposes_durable_progress(tmp_path) -> None:
    async def scenario() -> None:
        store = GatewayStore(tmp_path)

        async def sink(event):
            del event

        manager = OperationManager(store, sink)
        release = asyncio.Event()

        async def work():
            await manager.progress(
                "ota-op",
                stage="transferring",
                progress_permille=450,
                bytes_received=1024,
                bytes_total=4096,
            )
            await release.wait()
            return {"healthy": True}

        operation, created = await manager.submit(
            "device-a",
            Actor("agent", "test"),
            "firmware.ota",
            {},
            work,
            operation_id="ota-op",
        )
        assert created is True
        assert operation["status"] in {"queued", "running", "transferring"}
        await asyncio.sleep(0)
        current = store.operation("ota-op")
        assert current is not None
        assert current["status"] == "transferring"
        assert current["progress"]["progress_permille"] == 450
        release.set()
        for _ in range(20):
            await asyncio.sleep(0)
            current = store.operation("ota-op")
            if current and current["status"] == "succeeded":
                break
        assert current is not None
        assert current["result"] == {"healthy": True}
        assert current["progress"]["stage"] == "succeeded"
        assert current["progress"]["progress_permille"] == 1000
        store.close()

    asyncio.run(scenario())
