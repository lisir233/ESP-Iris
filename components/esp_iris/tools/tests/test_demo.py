from __future__ import annotations

import asyncio

from iris_gateway.demo import DemoHub


def test_demo_subscriber_observes_restart_and_can_unsubscribe() -> None:
    async def exercise() -> None:
        hub = DemoHub()
        device_id = "demo-a1b2c3d4"
        queue = hub.subscribe(device_id)
        try:
            await hub.restart(device_id, delay_ms=0)
            event = await asyncio.wait_for(queue.get(), 1)
            assert event["kind"] == "connection"
            assert event["connection_state"] == "rebooted"
            assert event["device_id"] == device_id

            hub.unsubscribe(device_id, queue)
            await hub.restart(device_id, delay_ms=0)
            assert queue.empty()
        finally:
            hub.unsubscribe(device_id, queue)
            await hub.close()

    asyncio.run(exercise())
