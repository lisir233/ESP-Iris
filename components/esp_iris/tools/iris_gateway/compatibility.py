"""Explicit firmware compatibility checks before any update-side mutation."""

from __future__ import annotations

from typing import Any

from .firmware import ESP32S31_CHIP_ID

TEXT_FIELDS = ("chip_target", "product_contract", "board_id", "layout_id")
CONTRACT_FIELDS = (*TEXT_FIELDS, "recovery_abi")


def compatibility_expectation(value: Any = None) -> dict[str, Any]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise TypeError("compatibility must be a JSON object")
    unknown = set(value) - set(CONTRACT_FIELDS)
    if unknown:
        raise ValueError(f"unknown compatibility fields: {sorted(unknown)}")
    result: dict[str, Any] = {}
    for key in CONTRACT_FIELDS:
        if key not in value:
            continue
        item = value[key]
        if key == "recovery_abi":
            if type(item) is not int or not 1 <= item <= 65535:
                raise ValueError("compatibility recovery_abi must be 1..65535")
        elif not isinstance(item, str) or not item or len(item.encode("utf-8")) > 64:
            raise ValueError(f"compatibility {key} must contain 1..64 UTF-8 bytes")
        result[key] = item
    return result


def validate_update_compatibility(
    status: dict[str, Any],
    chip_id: Any,
    expectation: dict[str, Any] | None = None,
    *,
    recovery: bool = False,
) -> dict[str, Any]:
    """Return the declared contract to require again after entering Recovery.

    Role and chip are always required. Generic images may omit product-specific
    declarations, but explicit caller expectations and any existing declarations
    must match, including across the normal-to-Recovery transition.
    """
    role = status.get("firmware_mode")
    if role not in ("normal", "recovery") or (recovery and role != "recovery"):
        raise ValueError("update requires an explicit compatible firmware role; migrate legacy firmware with product recovery")
    if type(chip_id) is not int or chip_id != ESP32S31_CHIP_ID:
        raise ValueError("update artifact has an unsupported or missing chip ID")
    required = compatibility_expectation(expectation)
    if required.get("chip_target", "esp32s31") != "esp32s31":
        raise ValueError("compatibility chip_target does not match the artifact")
    required["chip_target"] = "esp32s31"
    for key, expected in required.items():
        actual = status.get(key)
        if actual != expected or type(actual) is not type(expected):
            raise ValueError(f"device compatibility {key} is missing or mismatched: expected {expected!r}")
    # Carry declarations forward even without caller expectations. A reconnect
    # must not silently switch product/board/layout/ABI under the same Device ID.
    for key in CONTRACT_FIELDS:
        actual = status.get(key)
        if actual not in (None, "", 0):
            required.setdefault(key, actual)
    return compatibility_expectation(required)
