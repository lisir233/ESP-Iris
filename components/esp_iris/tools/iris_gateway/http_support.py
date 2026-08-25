"""Shared HTTP adapter primitives; contains no device/Gateway workflows."""

from __future__ import annotations

import contextvars
import ipaddress
from typing import Any

from aiohttp import web

from .security import Actor


PUBLIC_API = {"/v1/health", "/v1/auth/state", "/v1/auth/setup", "/v1/auth/login"}
ACTOR_CONTEXT: contextvars.ContextVar[Actor | None] = contextvars.ContextVar(
    "esp_iris_actor", default=None
)


def request_is_loopback(request: web.Request) -> bool:
    remote = request.remote
    if not remote:
        return False
    try:
        address = ipaddress.ip_address(remote.split("%", 1)[0])
        if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
            address = address.ipv4_mapped
        return address.is_loopback
    except ValueError:
        return False


def error_response(
    status: int, code: str, message: str, **details: Any
) -> web.Response:
    body: dict[str, Any] = {"error": {"code": code, "message": message}}
    if details:
        body["error"]["details"] = details
    return web.json_response(body, status=status)


def request_actor(request: web.Request) -> Actor:
    del request
    actor = ACTOR_CONTEXT.get()
    if not isinstance(actor, Actor):
        raise web.HTTPUnauthorized()
    return actor


async def json_body(request: web.Request) -> dict[str, Any]:
    if not request.can_read_body:
        return {}
    value = await request.json()
    if not isinstance(value, dict):
        raise TypeError("JSON request body must be an object")
    return value


__all__ = [
    "ACTOR_CONTEXT",
    "PUBLIC_API",
    "error_response",
    "json_body",
    "request_actor",
    "request_is_loopback",
]
