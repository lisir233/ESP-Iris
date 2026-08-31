from __future__ import annotations

import os
import pathlib
import subprocess
import time
from collections.abc import Mapping, Sequence

from .artifacts import ArtifactStore


class CommandError(RuntimeError):
    pass


class CommandRunner:
    def __init__(self, artifacts: ArtifactStore) -> None:
        self.artifacts = artifacts

    def run(
        self,
        argv: Sequence[str | os.PathLike[str]],
        *,
        cwd: pathlib.Path | None = None,
        env: Mapping[str, str] | None = None,
        timeout: float = 1200,
        log_name: str | None = None,
        check: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        normalized = [os.fspath(item) for item in argv]
        started = time.monotonic()
        result = subprocess.run(
            normalized,
            check=False,
            cwd=cwd,
            env=dict(env) if env is not None else None,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            errors="replace",
        )
        duration = time.monotonic() - started
        output = self.artifacts.redact(result.stdout)
        if log_name:
            path = self.artifacts.logs / log_name
            path.write_text(output, encoding="utf-8")
        self.artifacts.record_command(normalized, result.returncode, duration)
        if check and result.returncode != 0:
            raise CommandError(
                f"command failed ({result.returncode}): {' '.join(normalized)}\n"
                + output[-4000:]
            )
        return subprocess.CompletedProcess(
            result.args, result.returncode, output, result.stderr
        )
