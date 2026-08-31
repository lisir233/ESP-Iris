# ESP-Iris local hardware E2E tests

These tests are destructive, single-board hardware tests. Ordinary `pytest` runs
collect them but skip every test unless `--iris-e2e` is present.

Run them from an initialized ESP-IDF PowerShell after setting the four required
secrets:

```powershell
$env:ESP_IRIS_E2E_WIFI_SSID = "..."
$env:ESP_IRIS_E2E_WIFI_PASSWORD = "..."
$env:ESP_IRIS_E2E_PAIRING_TOKEN = "<64 lowercase hex characters>"
$env:ESP_IRIS_E2E_NEXT_PAIRING_TOKEN = "<a different 64 lowercase hex token>"

cd components/esp_iris/tools
python -m pytest tests/e2e -m iris_e2e --iris-e2e `
  --iris-chip-mac 30:ed:a0:f4:0c:28 `
  --iris-program-port COM8
```

Use `--iris-app-port COM13` only when automatic USB CDC discovery is unsuitable.
Use `--iris-artifacts <path>` to override the default
`test_results/e2e/<UTC-run-id>/` evidence directory.

## Safety and recovery

Before the first flash, the runner builds every profile, verifies its sdkconfig,
partition headroom, application descriptor and image hash, then checks the exact
ESP32-S31 MAC and flash capacity. It opens both serial endpoints exclusively and
backs up the NVS partition before recording a recovery journal. A failed backup
prevents all flashing.

The session finalizer restores the `services_usb` 2 MB layout and original NVS,
then verifies the original stable device ID and a clean smoke test. A later run
detecting an unfinished journal performs that same recovery after rechecking the
physical MAC. Pairing credentials and private TCP firmware are confined to the
run's `private/` area and temporary build directory; logs and shared evidence are
redacted.

## Stages

Tests execute in `iris_stage` order: preflight/build, disabled and USB core,
services/media/files, USB Serial/JTAG, TCP pairing, OTA, crash recovery, and
Gateway/CLI/TLS/Workbench. Each independent stage flashes its own baseline.
OTA and crash state transitions remain inside one test so a failure cannot make
another test depend on half-completed device state.
