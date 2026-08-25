import ctypes
import pathlib
import shutil
import subprocess

import pytest

from iris_gateway.state_machine import (
    OperationState,
    SessionEvent,
    SessionState,
    StateTransitionError,
    operation_transition,
    session_transition,
)

COMPONENT = pathlib.Path(__file__).resolve().parents[2]


def test_operation_state_machine_accepts_progress_and_freezes_terminal_state() -> None:
    assert operation_transition("queued", "running") is OperationState.RUNNING
    assert operation_transition("running", "transferring") is OperationState.TRANSFERRING
    assert operation_transition("transferring", "transferring") is OperationState.TRANSFERRING
    assert operation_transition("transferring", "succeeded") is OperationState.SUCCEEDED
    with pytest.raises(StateTransitionError):
        operation_transition("succeeded", "running")


def test_session_state_machine_rejects_ready_without_authentication() -> None:
    assert (
        session_transition(SessionState.NEGOTIATING, SessionEvent.AUTHENTICATED)
        is SessionState.READY
    )
    assert session_transition(SessionState.READY, SessionEvent.CLOSE) is SessionState.CLOSED
    with pytest.raises(StateTransitionError):
        session_transition(SessionState.CLOSED, SessionEvent.AUTHENTICATED)


def test_device_c_state_machine_has_the_same_terminal_guards(tmp_path) -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("a C compiler is required for device state-machine tests")
    output = tmp_path / "libiris_state.so"
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
            str(COMPONENT / "src" / "esp_iris_state.c"),
            "-o",
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    library = ctypes.CDLL(str(output))
    transition = library.iris_lifecycle_transition
    transition.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    transition.restype = ctypes.c_bool
    state = ctypes.c_int()
    assert transition(0, 1, ctypes.byref(state)) and state.value == 1
    assert transition(1, 2, ctypes.byref(state)) and state.value == 2
    assert not transition(2, 0, ctypes.byref(state))

