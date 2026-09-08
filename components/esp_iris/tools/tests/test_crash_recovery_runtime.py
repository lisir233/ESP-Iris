from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

COMPONENT = Path(__file__).resolve().parents[2]
HOST = Path(__file__).parent / "runtime_host"


def test_crash_recovery_state_machine(tmp_path: Path) -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        if os.environ.get("IRIS_REQUIRE_HOST_CC") == "1":
            pytest.fail("required C compiler is missing")
        pytest.skip("C compiler required for crash-recovery runtime test")
    output = tmp_path / "crash_recovery_runtime"
    build = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-include",
            str(HOST / "sdkconfig.h"),
            "-I",
            str(HOST),
            "-I",
            str(COMPONENT / "include"),
            "-I",
            str(COMPONENT / "src"),
            str(HOST / "crash_recovery_test.c"),
            "-o",
            str(output),
        ],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run(
        [str(output)], capture_output=True, text=True, timeout=10, check=False
    )
    assert run.returncode == 0, run.stdout + run.stderr
