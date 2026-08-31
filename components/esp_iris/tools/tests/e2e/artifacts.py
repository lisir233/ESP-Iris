from __future__ import annotations

import hashlib
import json
import os
import pathlib
import threading
from contextlib import suppress
from datetime import UTC, datetime
from typing import Any


class ArtifactStore:
    def __init__(self, root: pathlib.Path, redactions: tuple[str, ...]) -> None:
        self.root = root
        self.private = root / "private"
        self.logs = root / "logs"
        self.responses = root / "responses"
        for path in (self.root, self.private, self.logs, self.responses):
            path.mkdir(parents=True, exist_ok=True)
        with suppress(PermissionError):
            os.chmod(self.private, 0o700)
        self._redactions = tuple(value for value in redactions if value)
        self._lock = threading.Lock()
        self._manifest: dict[str, Any] = {
            "schema": "esp-iris-e2e-run/v1",
            "started_at": datetime.now(UTC).isoformat(),
            "status": "running",
            "commands": [],
            "profiles": {},
            "stages": [],
            "sensitive_artifacts": ["private/nvs.bin"],
        }
        self.flush_manifest()

    def redact(self, value: str) -> str:
        for secret in self._redactions:
            value = value.replace(secret, "<redacted>")
        return value

    def record_command(
        self, argv: list[str], returncode: int, duration_seconds: float
    ) -> None:
        with self._lock:
            self._manifest["commands"].append(
                {
                    "argv": [self.redact(item) for item in argv],
                    "returncode": returncode,
                    "duration_seconds": round(duration_seconds, 3),
                }
            )
            self.flush_manifest()

    def record_profile(self, name: str, values: dict[str, Any]) -> None:
        with self._lock:
            self._manifest["profiles"][name] = values
            self.flush_manifest()

    def record_stage(self, name: str, status: str, **details: Any) -> None:
        with self._lock:
            self._manifest["stages"].append(
                {"name": name, "status": status, **details}
            )
            self.flush_manifest()

    def finish(self, status: str) -> None:
        with self._lock:
            self._manifest["status"] = status
            self._manifest["finished_at"] = datetime.now(UTC).isoformat()
            self.flush_manifest()

    def flush_manifest(self) -> None:
        temporary = self.root / "manifest.json.tmp"
        temporary.write_text(
            json.dumps(self._manifest, indent=2, sort_keys=True), encoding="utf-8"
        )
        temporary.replace(self.root / "manifest.json")

    def write_json(self, relative: str, value: Any) -> pathlib.Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2, sort_keys=True), encoding="utf-8")
        return path

    @staticmethod
    def sha256(path: pathlib.Path) -> str:
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
