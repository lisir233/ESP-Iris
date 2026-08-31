from __future__ import annotations

import asyncio
import json
import pathlib
import time
from collections.abc import Callable, Coroutine
from typing import Any, TypeVar

T = TypeVar("T")


def run(coroutine: Coroutine[Any, Any, T]) -> T:
    return asyncio.run(coroutine)


def wait_for(
    predicate: Callable[[], T | None], *, timeout: float = 30, interval: float = 0.1
) -> T:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value is not None:
            return value
        time.sleep(interval)
    raise TimeoutError("condition did not converge")


def application_artifacts(build_dir: pathlib.Path) -> tuple[pathlib.Path, ...]:
    description = json.loads(
        (build_dir / "project_description.json").read_text(encoding="utf-8")
    )
    binary = build_dir / pathlib.Path(str(description["app_bin"])).name
    elf = pathlib.Path(str(description["app_elf"]))
    if not elf.is_absolute():
        elf = build_dir / elf.name
    maps = sorted(build_dir.glob("*.map"))
    if not binary.is_file() or not elf.is_file() or not maps:
        raise FileNotFoundError(f"incomplete application artifacts in {build_dir}")
    return binary, elf, maps[0]
