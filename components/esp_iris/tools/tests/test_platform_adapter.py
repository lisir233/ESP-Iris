from __future__ import annotations

import ctypes
import os
import pathlib
import shutil
import subprocess

import pytest

COMPONENT = pathlib.Path(__file__).resolve().parents[2]


def test_default_platform_adapter_is_safe_and_never_marks_healthy(tmp_path) -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("a C compiler is required for platform-adapter tests")
    output = tmp_path / ("iris_platform.dll" if os.name == "nt" else "libiris_platform.so")
    # MinGW cannot directly export weak symbols. Strong test-only wrappers call
    # the production defaults without changing their overridable linkage.
    wrapper = tmp_path / "platform_wrapper.c"
    wrapper.write_text(
        '#include "esp_iris_platform.c"\n'
        'esp_err_t iris_test_mark_healthy(void) {\n'
        '    return esp_iris_platform_mark_healthy();\n}\n'
        'esp_err_t iris_test_select(uint32_t current, uint32_t *target) {\n'
        '    return esp_iris_platform_select_ota_target(current, target);\n}\n'
    )
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-shared",
            "-fPIC",
            "-I",
            str(pathlib.Path(__file__).parent / "host_include"),
            "-I",
            str(COMPONENT / "include"),
            "-I",
            str(COMPONENT / "src"),
            str(wrapper),
            "-o",
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    library = ctypes.CDLL(str(output))
    assert library.iris_test_mark_healthy() == 0x106
    target = ctypes.c_uint32()
    select = library.iris_test_select
    select.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    assert select(0x220000, ctypes.byref(target)) == 0
    assert target.value == 0x220000

