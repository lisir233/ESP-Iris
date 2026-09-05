"""Compile the complete device runtime/services with deterministic hardware stubs."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess

import pytest

COMPONENT = Path(__file__).resolve().parents[2]
HOST = Path(__file__).parent / "runtime_host"


@pytest.mark.parametrize("service_profile", ["none", "ota", "inventory", "system-update"])
@pytest.mark.parametrize("multi_transport", [False, True])
def test_firmware_runtime(tmp_path: Path, multi_transport: bool, service_profile: str) -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("C compiler required for production firmware regression tests")
    output = tmp_path / ("runtime.exe" if os.name == "nt" else "runtime")
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
    if os.environ.get("IRIS_HOST_SANITIZERS") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    if service_profile in {"ota", "system-update"}:
        flags += ["-DCONFIG_ESP_IRIS_OTA=1"]
    if service_profile in {"inventory", "system-update"}:
        flags += ["-DCONFIG_ESP_IRIS_SYSTEM_INVENTORY=1"]
    if service_profile == "system-update":
        flags += ["-DCONFIG_ESP_IRIS_SYSTEM_UPDATE=1"]
    if multi_transport:
        flags += ["-DCONFIG_ESP_IRIS_TRANSPORT_USB=1"]
    command = [compiler, *flags, "-include", str(HOST / "sdkconfig.h"),
               "-I", str(HOST), "-I", str(COMPONENT / "include"),
               "-I", str(COMPONENT / "src"), str(HOST / "runtime_test.c"),
               str(COMPONENT / "src" / "esp_iris_codec.c"),
               str(COMPONENT / "src" / "esp_iris_state.c"), "-o", str(output)]
    build = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, run.stdout + run.stderr
