"""Small dependency-free metrics and event-envelope primitives."""

from __future__ import annotations

import collections
import dataclasses
import time
from typing import Any


CORRELATION_FIELDS = (
    "device_id",
    "boot_id",
    "session_id",
    "operation_id",
    "request_id",
    "job_id",
    "firmware_sha256",
)


def normalize_event(
    event: dict[str, Any], *, category: str, component: str
) -> dict[str, Any]:
    """Add a stable envelope without changing existing event payload fields."""

    item = dict(event)
    now = time.time_ns()
    item.setdefault("schema", "esp-iris-event/v1")
    item.setdefault("host_receive_wall_ns", now)
    item.setdefault("component", component)
    item.setdefault("event_name", str(item.get("kind", category)))
    item.setdefault("severity", "info")
    item["correlation"] = {
        name: item[name]
        for name in CORRELATION_FIELDS
        if item.get(name) is not None
    }
    return item


@dataclasses.dataclass
class _Distribution:
    count: int = 0
    total: float = 0.0
    maximum: float = 0.0

    def observe(self, value: float) -> None:
        self.count += 1
        self.total += value
        self.maximum = max(self.maximum, value)

    def as_dict(self) -> dict[str, float | int]:
        return {
            "count": self.count,
            "sum": self.total,
            "max": self.maximum,
            "average": self.total / self.count if self.count else 0.0,
        }


class MetricsRegistry:
    """Process-local metrics exported as stable JSON by the Gateway."""

    def __init__(self) -> None:
        self.started_monotonic_ns = time.monotonic_ns()
        self._counters: collections.Counter[str] = collections.Counter()
        self._gauges: dict[str, float] = {}
        self._distributions: dict[str, _Distribution] = {}

    def increment(self, name: str, amount: int = 1) -> None:
        if amount < 0:
            raise ValueError("counter increments must be non-negative")
        self._counters[name] += amount

    def gauge(self, name: str, value: float) -> None:
        self._gauges[name] = float(value)

    def observe(self, name: str, value: float) -> None:
        if value < 0:
            raise ValueError("observations must be non-negative")
        self._distributions.setdefault(name, _Distribution()).observe(float(value))

    def snapshot(self) -> dict[str, Any]:
        return {
            "schema": "esp-iris-metrics/v1",
            "uptime_seconds": (
                time.monotonic_ns() - self.started_monotonic_ns
            ) / 1_000_000_000,
            "counters": dict(sorted(self._counters.items())),
            "gauges": dict(sorted(self._gauges.items())),
            "distributions": {
                name: value.as_dict()
                for name, value in sorted(self._distributions.items())
            },
        }


__all__ = ["CORRELATION_FIELDS", "MetricsRegistry", "normalize_event"]
