from __future__ import annotations

import asyncio
import csv
import json
import pathlib
import re
import shutil
import tempfile
import time
from typing import Any

from iris_gateway.discovery import discover_iris_usb_devices

from .artifacts import ArtifactStore
from .config import PROFILES, ROOT, E2EConfig, FirmwareProfile
from .raw import RawIrisSession
from .runner import CommandRunner

MAC_OUTPUT = re.compile(
    r"MAC:\s*((?:[0-9a-f]{2}:){5}[0-9a-f]{2})", re.IGNORECASE
)
FLASH_SIZE = re.compile(
    r"(?:Detected flash size|Flash size):\s*(\d+)\s*MB", re.IGNORECASE
)


class SafetyError(RuntimeError):
    pass


class BoardController:
    NVS_OFFSET = "0x9000"
    NVS_SIZE = "0x6000"

    def __init__(
        self,
        config: E2EConfig,
        artifacts: ArtifactStore,
        runner: CommandRunner,
    ) -> None:
        self.config = config
        self.artifacts = artifacts
        self.runner = runner
        self.idf = shutil.which("idf.py")
        self.esptool = shutil.which("esptool.py") or shutil.which("esptool")
        self.build_root = ROOT / "build-e2e" / config.run_id
        self.build_root.mkdir(parents=True, exist_ok=True)
        self._private_build = tempfile.TemporaryDirectory(
            prefix="esp-iris-e2e-private-"
        )
        self.private_build_root = pathlib.Path(self._private_build.name)
        self.journal = ROOT / "test_results" / "e2e" / "recovery-journal.json"
        self.nvs_backup = artifacts.private / "nvs.bin"
        self._built: dict[str, pathlib.Path] = {}
        self.mutation_started = False
        self.original_device_id: str | None = None

    def close(self) -> None:
        self._private_build.cleanup()

    def preflight(self) -> dict[str, Any]:
        if self.idf is None:
            raise SafetyError("idf.py is not available; run from an initialized ESP-IDF shell")
        if self.esptool is None:
            raise SafetyError("esptool.py/esptool is not available")
        chip_result = self.runner.run(
            [self.esptool, "--port", self.config.program_port, "chip_id"],
            timeout=30,
            log_name="preflight-chip-id.log",
        )
        if "ESP32-S31" not in chip_result.stdout.upper():
            raise SafetyError("selected port is not an ESP32-S31")
        result = self.runner.run(
            [self.esptool, "--port", self.config.program_port, "flash_id"],
            timeout=30,
            log_name="preflight-flash-id.log",
        )
        mac_result = self.runner.run(
            [self.esptool, "--port", self.config.program_port, "read_mac"],
            timeout=30,
            log_name="preflight-read-mac.log",
        )
        match = MAC_OUTPUT.search(mac_result.stdout)
        if match is None:
            raise SafetyError("could not read target MAC")
        actual_mac = match.group(1).lower()
        if actual_mac != self.config.chip_mac:
            raise SafetyError(
                f"target MAC mismatch: expected {self.config.chip_mac}, got {actual_mac}"
            )
        flash_match = FLASH_SIZE.search(result.stdout)
        if flash_match is None or int(flash_match.group(1)) < 16:
            raise SafetyError("the destructive matrix requires at least 16 MB flash")
        self._probe_port(self.config.program_port)
        if self.config.app_port:
            self._probe_port(self.config.app_port)
        return {
            "chip": "ESP32-S31",
            "mac": actual_mac,
            "flash_mb": int(flash_match.group(1)),
            "program_port": self.config.program_port,
            "app_port": self.config.app_port,
        }

    @staticmethod
    def _probe_port(port: str) -> None:
        import serial

        try:
            handle = serial.Serial(port=port, baudrate=115200, timeout=0.1)
        except Exception as exc:
            raise SafetyError(f"serial endpoint is unavailable or busy: {port}") from exc
        handle.close()

    def backup_nvs(self) -> str:
        assert self.esptool is not None
        self.runner.run(
            [
                self.esptool,
                "--port",
                self.config.program_port,
                "read_flash",
                self.NVS_OFFSET,
                self.NVS_SIZE,
                self.nvs_backup,
            ],
            timeout=120,
            log_name="backup-nvs.log",
        )
        if self.nvs_backup.stat().st_size != int(self.NVS_SIZE, 0):
            raise SafetyError("NVS backup has an unexpected size")
        digest = self.artifacts.sha256(self.nvs_backup)
        self._write_journal("nvs-backed-up", nvs_sha256=digest)
        return digest

    def restore_nvs(self) -> None:
        self.restore_nvs_from(self.nvs_backup)

    def restore_nvs_from(self, backup: pathlib.Path) -> None:
        assert self.esptool is not None
        if not backup.exists():
            raise SafetyError("cannot restore missing NVS backup")
        self.runner.run(
            [
                self.esptool,
                "--port",
                self.config.program_port,
                "write_flash",
                self.NVS_OFFSET,
                backup,
            ],
            timeout=120,
            log_name="restore-nvs.log",
        )

    def unfinished_journal(self) -> dict[str, Any] | None:
        if not self.journal.exists():
            return None
        try:
            value = json.loads(self.journal.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise SafetyError("recovery journal is unreadable") from exc
        if value.get("schema") != "esp-iris-e2e-recovery/v1":
            raise SafetyError("recovery journal has an unsupported schema")
        if str(value.get("chip_mac", "")).lower() != self.config.chip_mac:
            raise SafetyError("unfinished recovery journal belongs to another board")
        return value

    def recover_unfinished(self, journal: dict[str, Any]) -> bool:
        if journal.get("state") == "nvs-backed-up":
            self.clear_journal()
            return False
        backup = pathlib.Path(str(journal.get("nvs_backup", "")))
        if backup.stat().st_size != int(self.NVS_SIZE, 0):
            raise SafetyError("unfinished run NVS backup has an unexpected size")
        expected = str(journal.get("nvs_sha256", ""))
        if expected and self.artifacts.sha256(backup) != expected:
            raise SafetyError("unfinished run NVS backup hash mismatch")
        shutil.copy2(backup, self.nvs_backup)
        self.flash("services_usb")
        self.restore_nvs_from(backup)
        self.clear_journal()
        return True

    def _write_journal(self, state: str, **details: Any) -> None:
        self.journal.parent.mkdir(parents=True, exist_ok=True)
        value = {
            "schema": "esp-iris-e2e-recovery/v1",
            "state": state,
            "chip_mac": self.config.chip_mac,
            "program_port": self.config.program_port,
            "run_root": str(self.artifacts.root),
            "nvs_backup": str(self.nvs_backup),
            **details,
        }
        temporary = self.journal.with_suffix(".tmp")
        temporary.write_text(json.dumps(value, indent=2), encoding="utf-8")
        temporary.replace(self.journal)

    def clear_journal(self) -> None:
        self.journal.unlink(missing_ok=True)

    def build(self, profile_name: str) -> pathlib.Path:
        if profile_name in self._built:
            return self._built[profile_name]
        profile = PROFILES[profile_name]
        build_dir = (
            self.private_build_root / profile.name
            if profile.private
            else self.build_root / profile.name
        )
        build_dir.mkdir(parents=True, exist_ok=True)
        sdkconfig = build_dir / "sdkconfig"
        defaults = [profile.project / item for item in profile.defaults]
        private_defaults: pathlib.Path | None = None
        if profile.private:
            private_defaults = build_dir / "sdkconfig.secrets.defaults"
            private_defaults.write_text(
                "\n".join(
                    [
                        f'CONFIG_ESP_IRIS_TEST_WIFI_SSID="{_kconfig(self.config.secrets.ssid)}"',
                        f'CONFIG_ESP_IRIS_TEST_WIFI_PASSWORD="{_kconfig(self.config.secrets.password)}"',
                        f'CONFIG_ESP_IRIS_TEST_PAIRING_TOKEN="{self.config.secrets.pairing_token}"',
                        f'CONFIG_ESP_IRIS_TEST_NEXT_PAIRING_TOKEN="{self.config.secrets.next_pairing_token}"',
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            defaults.append(private_defaults)
        joined_defaults = ";".join(str(item) for item in defaults)
        assert self.idf is not None
        self.runner.run(
            [
                self.idf,
                "-C",
                profile.project,
                "-B",
                build_dir,
                "-D",
                f"SDKCONFIG={sdkconfig}",
                "-D",
                f"SDKCONFIG_DEFAULTS={joined_defaults}",
                "build",
            ],
            cwd=ROOT,
            timeout=1800,
            log_name=f"build-{profile.name}.log",
        )
        self._validate_profile(profile, build_dir)
        self._built[profile_name] = build_dir
        return build_dir

    def _validate_profile(
        self, profile: FirmwareProfile, build_dir: pathlib.Path
    ) -> None:
        header = build_dir / "config" / "sdkconfig.h"
        content = header.read_text(encoding="utf-8", errors="replace")
        missing = [macro for macro in profile.expected_macros if macro not in content]
        if missing:
            raise SafetyError(
                f"{profile.name} built with the wrong profile: missing {missing}"
            )
        present = [macro for macro in profile.absent_macros if macro in content]
        if present:
            raise SafetyError(
                f"{profile.name} built with the wrong profile: present {present}"
            )
        app_bins = [
            path
            for path in build_dir.glob("*.bin")
            if path.name not in {"bootloader.bin", "partition-table.bin"}
        ]
        if not app_bins:
            raise SafetyError(f"{profile.name} did not produce an application binary")
        description_path = build_dir / "project_description.json"
        description = json.loads(description_path.read_text(encoding="utf-8"))
        app_name = pathlib.Path(str(description.get("app_bin", ""))).name
        app = build_dir / app_name if app_name else max(
            app_bins, key=lambda path: path.stat().st_size
        )
        if not app.is_file():
            raise SafetyError(f"{profile.name} project descriptor names no app binary")
        partition_size = _minimum_app_partition_size(
            profile.project / "partitions.csv"
        )
        remaining = partition_size - app.stat().st_size
        if remaining < 64 * 1024:
            raise SafetyError(
                f"{profile.name} leaves only {remaining} bytes in its smallest "
                "application partition"
            )
        assert self.esptool is not None
        image_info = self.runner.run(
            [self.esptool, "image_info", app],
            timeout=30,
            log_name=f"image-info-{profile.name}.log",
        ).stdout
        project_name = str(description.get("project_name", ""))
        project_version = str(description.get("project_version", ""))
        for expected in (project_name, project_version):
            if expected and expected not in image_info:
                raise SafetyError(
                    f"{profile.name} image descriptor is missing {expected!r}"
                )
        self.artifacts.record_profile(
            profile.name,
            {
                "application": str(app),
                "bytes": app.stat().st_size,
                "partition_bytes": partition_size,
                "partition_remaining_bytes": remaining,
                "sha256": self.artifacts.sha256(app),
                "project_name": project_name,
                "project_version": project_version,
                "private": profile.private,
            },
        )

    def build_all(self) -> dict[str, pathlib.Path]:
        return {name: self.build(name) for name in PROFILES}

    def flash(self, profile_name: str) -> pathlib.Path:
        build_dir = self.build(profile_name)
        assert self.idf is not None
        # ESP-IDF's incremental flash cache cannot observe that an OTA test
        # changed otadata at runtime. Force that small mutable partition to be
        # written so every profile starts from its declared factory layout.
        (build_dir / "ota_data_initial_flashed.bin").unlink(missing_ok=True)
        self.mutation_started = True
        self._write_journal("flashing", profile=profile_name)
        self.runner.run(
            [
                self.idf,
                "-C",
                PROFILES[profile_name].project,
                "-B",
                build_dir,
                "-p",
                self.config.program_port,
                "flash",
            ],
            cwd=ROOT,
            timeout=600,
            log_name=f"flash-{profile_name}.log",
        )
        if profile_name == "services_usj":
            assert self.esptool is not None
            reset = self.runner.run(
                [
                    self.esptool,
                    "--port",
                    self.config.program_port,
                    "--after",
                    "hard-reset",
                    "run",
                ],
                timeout=30,
                log_name="flash-services_usj-reset.log",
                check=False,
            )
            known_usb_teardown = (
                "Hard resetting via RTS pin" in reset.stdout
                and "OSError: [Errno 71] Protocol error" in reset.stdout
            )
            if reset.returncode != 0 and not known_usb_teardown:
                raise SafetyError(
                    "USB Serial/JTAG firmware did not leave the bootloader; "
                    "see flash-services_usj-reset.log"
                )
        self._write_journal("profile-running", profile=profile_name)
        return build_dir

    def discover_application_port(self, timeout: float = 30) -> str:
        if self.config.app_port:
            return self.config.app_port
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ports = discover_iris_usb_devices(include_usb_serial_jtag=False)
            matches = [item.path for item in ports if item.transport == "usb"]
            if len(matches) == 1:
                return matches[0]
            time.sleep(0.25)
        raise SafetyError("application USB CDC endpoint was not discovered")

    def wait_console_marker(
        self,
        pattern: str,
        *,
        timeout: float = 45,
        reset: bool = True,
        log_name: str = "console.log",
    ) -> re.Match[str]:
        import serial

        if reset:
            assert self.esptool is not None
            self.runner.run(
                [self.esptool, "--port", self.config.program_port, "run"],
                timeout=30,
                log_name="console-reset.log",
            )
        expression = re.compile(pattern)
        deadline = time.monotonic() + timeout
        lines: list[str] = []
        with serial.Serial(
            port=self.config.program_port, baudrate=115200, timeout=0.2
        ) as console:
            while time.monotonic() < deadline:
                raw = console.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip()
                lines.append(self.artifacts.redact(line))
                match = expression.search(line)
                if match is not None:
                    (self.artifacts.logs / log_name).write_text(
                        "\n".join(lines) + "\n", encoding="utf-8"
                    )
                    return match
        (self.artifacts.logs / log_name).write_text(
            "\n".join(lines) + "\n", encoding="utf-8"
        )
        raise TimeoutError(f"console marker was not observed: {pattern}")

    def capture_baseline_identity(self) -> dict[str, Any]:
        async def capture() -> dict[str, Any]:
            raw = RawIrisSession(self.discover_application_port(15))
            info = await raw.open()
            try:
                assert raw.session is not None
                status = await raw.session.status()
                return {
                    "device_id": info.device_id,
                    "boot_id": info.boot_id,
                    "session_id": info.session_id,
                    "project_name": info.project_name,
                    "app_version": info.app_version,
                    "status": status,
                }
            finally:
                await raw.close()

        value = asyncio.run(capture())
        self.original_device_id = value["device_id"]
        return value

    def verify_restored_baseline(self) -> dict[str, Any]:
        async def chunks():
            yield b"restore smoke\n"

        async def verify() -> dict[str, Any]:
            raw = RawIrisSession(self.discover_application_port(30))
            info = await raw.open()
            try:
                if self.original_device_id is None or (
                    info.device_id != self.original_device_id
                ):
                    raise SafetyError(
                        "restored baseline device ID differs from the preflight identity"
                    )
                assert raw.session is not None
                status = await raw.session.status()
                if status["invalid_frames"] != 0 or status["log_dropped_bytes"] != 0:
                    raise SafetyError("restored baseline counters are not clean")
                assert await raw.session.rpc(1, 1, b"restore") == b"restore"
                description, pixels = await raw.session.screenshot()
                if description["width"] != 2 or len(pixels) != 8:
                    raise SafetyError("restored baseline screenshot smoke failed")
                payload = b"restore smoke\n"
                uploaded = await raw.session.files.upload(
                    "fs",
                    "e2e-restore-smoke.txt",
                    chunks(),
                    total_size=len(payload),
                    overwrite=True,
                )
                await raw.session.files.delete("fs", "e2e-restore-smoke.txt")
                return {
                    "device_id": info.device_id,
                    "boot_id": info.boot_id,
                    "invalid_frames": status["invalid_frames"],
                    "log_dropped_bytes": status["log_dropped_bytes"],
                    "file_sha256": uploaded["sha256"],
                }
            finally:
                await raw.close()

        return asyncio.run(verify())

    def restore_baseline(self) -> str:
        self.flash("services_usb")
        self.restore_nvs()
        details = self.verify_restored_baseline()
        self.artifacts.write_json("restore-smoke.json", details)
        self.clear_journal()
        return self.discover_application_port(30)


def _kconfig(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _minimum_app_partition_size(path: pathlib.Path) -> int:
    sizes: list[int] = []
    with path.open(newline="", encoding="utf-8") as source:
        for row in csv.reader(source):
            if not row or row[0].lstrip().startswith("#") or len(row) < 5:
                continue
            if row[1].strip() == "app":
                sizes.append(int(row[4].strip(), 0))
    if not sizes:
        raise SafetyError(f"partition table contains no application: {path}")
    return min(sizes)
