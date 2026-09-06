"""Add exact Boot ID display values without changing numeric API fields."""

from __future__ import annotations

from typing import Any


def boot_id_text(item: dict[str, Any]) -> dict[str, Any]:
    """Copy metadata, including nested evidence, with decimal Boot ID aliases."""
    result = {}
    for key, value in item.items():
        if isinstance(value, dict):
            value = boot_id_text(value)
        elif isinstance(value, list):
            value = [boot_id_text(entry) if isinstance(entry, dict) else entry for entry in value]
        result[key] = value
    for key, value in item.items():
        if (key == "boot_id" or key.endswith("_boot_id")) and type(value) is int:
            result[key + "_text"] = str(value)
    return result
