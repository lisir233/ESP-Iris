from __future__ import annotations

import pathlib

from .config import E2EConfig, WifiSecrets


def test_secret_values_are_not_exposed_by_fixture_repr() -> None:
    password = "wifi-password"
    pairing_token = "a" * 64
    next_pairing_token = "b" * 64
    secrets = WifiSecrets(
        ssid="test-network",
        password=password,
        pairing_token=pairing_token,
        next_pairing_token=next_pairing_token,
    )
    config = E2EConfig(
        chip_mac="30:ed:a0:f4:0c:28",
        program_port="COM8",
        app_port=None,
        artifacts=pathlib.Path("test-results"),
        run_id="test-run",
        secrets=secrets,
    )

    rendered = f"{secrets!r} {config!r}"
    assert password not in rendered
    assert pairing_token not in rendered
    assert next_pairing_token not in rendered
