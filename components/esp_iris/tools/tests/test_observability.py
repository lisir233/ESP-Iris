from __future__ import annotations

from iris_gateway.observability import MetricsRegistry, normalize_event


def test_event_envelope_preserves_payload_and_collects_correlation() -> None:
    event = normalize_event(
        {
            "kind": "operation",
            "device_id": "device-a",
            "operation_id": "op-1",
            "custom": 7,
        },
        category="operation",
        component="gateway",
    )
    assert event["schema"] == "esp-iris-event/v1"
    assert event["component"] == "gateway"
    assert event["custom"] == 7
    assert event["correlation"] == {
        "device_id": "device-a",
        "operation_id": "op-1",
    }


def test_metrics_registry_tracks_counters_gauges_and_distributions() -> None:
    metrics = MetricsRegistry()
    metrics.increment("events.device", 2)
    metrics.gauge("devices.connected", 1)
    metrics.observe("operations.duration_seconds", 0.25)
    metrics.observe("operations.duration_seconds", 0.75)
    snapshot = metrics.snapshot()
    assert snapshot["schema"] == "esp-iris-metrics/v1"
    assert snapshot["counters"]["events.device"] == 2
    assert snapshot["gauges"]["devices.connected"] == 1.0
    assert snapshot["distributions"]["operations.duration_seconds"] == {
        "count": 2,
        "sum": 1.0,
        "max": 0.75,
        "average": 0.5,
    }

