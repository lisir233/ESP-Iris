from __future__ import annotations

import pytest

pytestmark = [
    pytest.mark.iris_e2e,
    pytest.mark.iris_stage(1),
    pytest.mark.firmware_profile("services_disabled"),
]


def test_disabled_profile_keeps_every_public_entry_point_safe(
    iris_board, firmware_profile
) -> None:
    assert firmware_profile == "services_disabled"
    marker = iris_board.wait_console_marker(
        r"IRIS_DISABLED_STATE schema=1 safe=(?P<safe>[01]) "
        r"calls=(?P<calls>\d+) started=(?P<started>[01])",
        log_name="console-disabled.log",
    )
    assert marker.group("safe") == "1"
    assert int(marker.group("calls")) >= 20
    assert marker.group("started") == "0"
