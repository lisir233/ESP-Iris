import ctypes
import pathlib
import shutil
import subprocess

import pytest

COMPONENT = pathlib.Path(__file__).resolve().parents[2]


def test_default_platform_adapter_is_safe_and_never_marks_healthy(tmp_path) -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("a C compiler is required for platform-adapter tests")
    output = tmp_path / "libiris_platform.so"
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
            str(COMPONENT / "src" / "esp_iris_platform.c"),
            "-o",
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    library = ctypes.CDLL(str(output))
    assert library.esp_iris_platform_mark_healthy() == 0x106
    target = ctypes.c_uint32()
    select = library.esp_iris_platform_select_ota_target
    select.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    assert select(0x220000, ctypes.byref(target)) == 0
    assert target.value == 0x220000

