"""Compile the production file service and verify its lazy allocation owner."""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

COMPONENT = Path(__file__).resolve().parents[2]
HOST = Path(__file__).parent / "runtime_host"


def test_file_service_lifecycle(tmp_path: Path) -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        if os.environ.get("IRIS_REQUIRE_HOST_CC") == "1":
            pytest.fail("required C compiler is missing; file lifecycle test cannot run")
        pytest.skip("C compiler required for the file lifecycle test")
    output = tmp_path / ("files-lifecycle.exe" if os.name == "nt" else "files-lifecycle")
    flags = [
        "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections",
    ]
    if os.environ.get("IRIS_HOST_SANITIZERS") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    command = [
        compiler,
        *flags,
        "-include",
        str(HOST / "sdkconfig.h"),
        "-I",
        str(HOST),
        "-I",
        str(COMPONENT / "include"),
        "-I",
        str(COMPONENT / "src"),
        str(HOST / "files_lifecycle_test.c"),
        str(COMPONENT / "src" / "esp_iris_codec.c"),
        "-Wl,--gc-sections",
        "-o",
        str(output),
    ]
    build = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(output)], capture_output=True, text=True, timeout=30, check=False)
    assert run.returncode == 0, run.stdout + run.stderr
