"""Executable HTTP contract for the ESP-Iris Gateway adapter."""

from __future__ import annotations

from typing import Any


def build_openapi(auth_required: bool) -> dict[str, Any]:
    control_paths = {
        "/v1/devices/{device_id}/rpc/raw": "Raw RPC",
        "/v1/devices/{device_id}/restart": "Restart device",
        "/v1/devices/{device_id}/factory-recovery": "Enter factory recovery",
        "/v1/devices/{device_id}/ota": "Validated OTA",
        "/v1/devices/{device_id}/input": "Pointer or touch gesture",
        "/v1/devices/{device_id}/console": "Submit one console command line",
        "/v1/devices/{device_id}/screenshot": "Capture screenshot",
        "/v1/devices/{device_id}/mirror/start": "Start media mirror",
        "/v1/devices/{device_id}/mirror/stop": "Stop media mirror",
    }
    paths: dict[str, Any] = {
        "/v1/health": {"get": {"summary": "Gateway health"}},
        "/v1/auth/login": {"post": {"summary": "Developer password login"}},
        "/v1/devices": {"get": {"summary": "Connected and cached devices"}},
        "/v1/devices/{device_id}": {
            "get": {"summary": "Current or cached status"},
            "delete": {
                "summary": "Remove an offline device from inventory",
                "description": "Preserves operations, events, logs, and audit history.",
            },
        },
        "/v1/mode": {
            "get": {"summary": "Get global mode"},
            "put": {"summary": "Switch develop or observe mode"},
        },
        "/v1/events": {"get": {"summary": "Cursor-based event history"}},
        "/v1/events/ws": {"get": {"summary": "Resumable event WebSocket"}},
        "/v1/operations": {"get": {"summary": "Device operation records"}},
        "/v1/firmware-artifacts": {
            "get": {"summary": "Archived firmware bundles"},
            "post": {"summary": "Archive BIN, ELF and map as one validated bundle"},
        },
        "/v1/system-audit": {"get": {"summary": "Gateway system audit"}},
        "/v1/metrics": {"get": {"summary": "Gateway process metrics"}},
    }
    for path, summary in control_paths.items():
        paths[path] = {"post": {"summary": summary}}
    paths["/v1/devices/{device_id}/jobs/{job_id}"] = {
        "get": {"summary": "Query job"},
        "delete": {"summary": "Cancel job"},
    }
    document = {
        "openapi": "3.1.0",
        "info": {
            "title": "ESP-Iris Developer Gateway",
            "version": "1.0.0",
            "description": "Gateway-only device control and observation API.",
        },
        "servers": [{"url": "/"}],
        "components": {
            "securitySchemes": {
                "cookieAuth": {
                    "type": "apiKey",
                    "in": "cookie",
                    "name": "esp_iris_session",
                },
                "agentToken": {"type": "http", "scheme": "bearer"},
            }
        },
        "paths": paths,
    }
    if auth_required:
        document["security"] = [{"cookieAuth": []}, {"agentToken": []}]
    return document


__all__ = ["build_openapi"]
