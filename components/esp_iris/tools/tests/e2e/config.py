from __future__ import annotations

import dataclasses
import os
import pathlib
import re

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[5]
COMPONENT = ROOT / "components" / "esp_iris"
TOOLS = COMPONENT / "tools"
E2E_ROOT = TOOLS / "tests" / "e2e"

TOKEN_PATTERN = re.compile(r"[0-9a-f]{64}\Z")
MAC_PATTERN = re.compile(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\Z", re.IGNORECASE)


@dataclasses.dataclass(frozen=True, slots=True)
class WifiSecrets:
    ssid: str
    password: str = dataclasses.field(repr=False)
    pairing_token: str = dataclasses.field(repr=False)
    next_pairing_token: str = dataclasses.field(repr=False)

    @classmethod
    def from_environment(cls) -> WifiSecrets:
        names = {
            "ssid": "ESP_IRIS_E2E_WIFI_SSID",
            "password": "ESP_IRIS_E2E_WIFI_PASSWORD",
            "pairing_token": "ESP_IRIS_E2E_PAIRING_TOKEN",
            "next_pairing_token": "ESP_IRIS_E2E_NEXT_PAIRING_TOKEN",
        }
        values = {field: os.environ.get(name, "") for field, name in names.items()}
        missing = [names[field] for field, value in values.items() if not value]
        if missing:
            raise pytest.UsageError(
                "full HIL requires environment variables: " + ", ".join(missing)
            )
        for field in ("pairing_token", "next_pairing_token"):
            if TOKEN_PATTERN.fullmatch(values[field]) is None:
                raise pytest.UsageError(
                    f"{names[field]} must contain 64 lowercase hexadecimal characters"
                )
        if values["pairing_token"] == values["next_pairing_token"]:
            raise pytest.UsageError("pairing tokens must be different")
        return cls(**values)

    @property
    def redactions(self) -> tuple[str, ...]:
        return (
            self.ssid,
            self.password,
            self.pairing_token,
            self.next_pairing_token,
        )


@dataclasses.dataclass(frozen=True, slots=True)
class E2EConfig:
    chip_mac: str
    program_port: str
    app_port: str | None
    artifacts: pathlib.Path
    run_id: str
    secrets: WifiSecrets = dataclasses.field(repr=False)

    @classmethod
    def from_pytest(cls, request: pytest.FixtureRequest) -> E2EConfig:
        mac = (request.config.getoption("--iris-chip-mac") or "").lower()
        program_port = request.config.getoption("--iris-program-port") or ""
        if MAC_PATTERN.fullmatch(mac) is None:
            raise pytest.UsageError(
                "--iris-chip-mac is required and must be a colon-separated MAC"
            )
        if not program_port:
            raise pytest.UsageError("--iris-program-port is required")
        run_id = request.config._iris_e2e_run_id
        artifacts = request.config._iris_e2e_artifacts
        return cls(
            chip_mac=mac,
            program_port=program_port,
            app_port=request.config.getoption("--iris-app-port"),
            artifacts=artifacts,
            run_id=run_id,
            secrets=WifiSecrets.from_environment(),
        )


@dataclasses.dataclass(frozen=True, slots=True)
class FirmwareProfile:
    name: str
    project: pathlib.Path
    defaults: tuple[str, ...]
    expected_macros: tuple[str, ...]
    absent_macros: tuple[str, ...] = ()
    private: bool = False


PROFILES: dict[str, FirmwareProfile] = {
    "services_usb": FirmwareProfile(
        "services_usb",
        COMPONENT / "test_apps" / "services_usb",
        ("sdkconfig.defaults",),
        ("CONFIG_ESP_IRIS_TRANSPORT_USB",),
    ),
    "services_usj": FirmwareProfile(
        "services_usj",
        COMPONENT / "test_apps" / "services_usb",
        ("sdkconfig.usj.defaults",),
        ("CONFIG_ESP_IRIS_TRANSPORT_USB_SERIAL_JTAG",),
    ),
    "services_disabled": FirmwareProfile(
        "services_disabled",
        COMPONENT / "test_apps" / "services_usb",
        ("sdkconfig.disabled.defaults",),
        (),
        ("CONFIG_ESP_IRIS_ENABLE",),
    ),
    "coredump_tcp": FirmwareProfile(
        "coredump_tcp",
        COMPONENT / "test_apps" / "coredump",
        ("sdkconfig.defaults",),
        ("CONFIG_ESP_IRIS_TRANSPORT_TCP", "CONFIG_ESP_IRIS_TCP_PAIRING"),
        private=True,
    ),
    "ota_recovery": FirmwareProfile(
        "ota_recovery",
        COMPONENT / "examples" / "ota",
        ("sdkconfig.recovery.defaults",),
        ("CONFIG_ESP_IRIS_OTA_EXAMPLE_RECOVERY",),
    ),
    "ota_a": FirmwareProfile(
        "ota_a",
        COMPONENT / "examples" / "ota",
        ("sdkconfig.defaults",),
        ("CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY",),
    ),
    "ota_b": FirmwareProfile(
        "ota_b",
        COMPONENT / "examples" / "ota",
        ("sdkconfig.candidate.defaults",),
        ("CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY",),
    ),
    "ota_application": FirmwareProfile(
        "ota_application",
        COMPONENT / "examples" / "ota",
        ("sdkconfig.application.defaults",),
        ("CONFIG_ESP_IRIS_OTA",),
    ),
    "ota_rollback": FirmwareProfile(
        "ota_rollback",
        COMPONENT / "examples" / "ota",
        ("sdkconfig.rollback.defaults",),
        ("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE",),
    ),
    "ota_fallback": FirmwareProfile(
        "ota_fallback",
        COMPONENT / "examples" / "ota",
        ("sdkconfig.fallback.defaults",),
        ("CONFIG_ESP_IRIS_OTA_ALLOW_APPLICATION_FALLBACK",),
    ),
    "ota_project_match": FirmwareProfile(
        "ota_project_match",
        COMPONENT / "examples" / "ota",
        ("sdkconfig.project-match.defaults",),
        ("CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH",),
    ),
    "crash_recovery": FirmwareProfile(
        "crash_recovery",
        COMPONENT / "examples" / "crash_recovery",
        ("sdkconfig.recovery.defaults",),
        ("CONFIG_ESP_IRIS_CRASH_EXAMPLE_RECOVERY",),
    ),
    "crash_application": FirmwareProfile(
        "crash_application",
        COMPONENT / "examples" / "crash_recovery",
        ("sdkconfig.application.defaults",),
        ("CONFIG_ESP_IRIS_CRASH_EXAMPLE_AUTO_CRASH",),
    ),
    "crash_application_stable": FirmwareProfile(
        "crash_application_stable",
        COMPONENT / "examples" / "crash_recovery",
        ("sdkconfig.stable.defaults",),
        ("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE",),
        ("CONFIG_ESP_IRIS_CRASH_EXAMPLE_AUTO_CRASH",),
    ),
}
