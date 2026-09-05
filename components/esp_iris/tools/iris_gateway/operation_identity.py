"""Versioned request identities; never include timestamps or mutable result state."""

from __future__ import annotations

import hashlib
import json
from typing import Any


class OperationConflict(RuntimeError):
    """An operation ID is already bound to another (or unverifiable legacy) request."""


def _canonical(value: Any) -> Any:
    if isinstance(value, bytes):
        return ["bytes", len(value), hashlib.sha256(value).hexdigest()]
    if isinstance(value, dict):
        if not all(isinstance(key, str) for key in value):
            raise ValueError("operation parameter keys must be strings")
        return ["object", [[key, _canonical(value[key])] for key in sorted(value)]]
    if isinstance(value, (tuple, list)):
        return ["array", [_canonical(item) for item in value]]
    if value is None or isinstance(value, (str, int, float, bool)):
        return [type(value).__name__, value]
    raise ValueError("unsupported operation parameter type")


def request_fingerprint(request: dict[str, Any]) -> str:
    identity = {key: request.get(key) for key in (
        "device_id", "actor_type", "actor_name", "actor_scopes", "action", "params"
    )}
    encoded = json.dumps(_canonical(identity), sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False, allow_nan=False).encode("utf-8")
    return "sha256:v1:" + hashlib.sha256(encoded).hexdigest()


def require_same_request(existing: dict[str, Any], fingerprint: str) -> None:
    if existing.get("request_fingerprint") != fingerprint:
        raise OperationConflict(
            "operation ID already belongs to a different request or a legacy request "
            "without a verifiable fingerprint; use a new operation ID"
        )
