# ESP-Iris Developer Gateway and Web Workbench

The Developer Gateway is the PC-side fleet hub for ESP-Iris. It supervises
multiple USB and TCP device endpoints concurrently, identifies devices by
stable `device_id`, exposes REST/WebSocket APIs, stores durable engineering
records, and serves the React Web Workbench. The `ctl` command-line client and
external agents use the same API concurrently with the Workbench.

Each physical device has at most one active Gateway session, but one Gateway
can own many device sessions. Every Workbench, CLI follow command, or external
agent WebSocket receives an independent event stream. Device-changing
operations are serialized per device; observers do not compete for events.

The Gateway is source distributed with the ESP-IDF component. It is not
installed as a Python package.

## Requirements

- Python 3.11 or newer
- Linux, macOS, or Windows; real-board validation currently focuses on Linux
- Node.js and npm when the Workbench must be built from source
- A dedicated serial endpoint for USB transports

Set one path for the rest of this guide:

```bash
# Source checkout
ESP_IRIS_COMPONENT_DIR=components/esp_iris

# Component Manager installation
# ESP_IRIS_COMPONENT_DIR=managed_components/lisir233__esp_iris
```

PowerShell users can set the equivalent `$env:ESP_IRIS_COMPONENT_DIR` value and
use `python` when it is the Python 3 launcher.

## Install the Gateway

Create an isolated environment and install the runtime dependencies:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r "$ESP_IRIS_COMPONENT_DIR/tools/requirements.txt"
```

The Registry archive includes the Workbench source but excludes generated
`node_modules` and `dist` directories. Build it once before starting the
Gateway:

```bash
cd "$ESP_IRIS_COMPONENT_DIR/tools/frontend"
npm ci
npm run build
cd -
```

Rebuild after changing frontend source. The Gateway displays a clear fallback
page when `dist/index.html` is absent.

## Evaluate without hardware

Demo mode creates virtual normal/recovery devices and exercises logs, RPC/jobs,
screen/input, media, OTA, restart, disconnect, and crash evidence:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web --demo
```

Open `http://127.0.0.1:8443/`.

## Connect devices

USB and TCP options are repeatable and may be combined. For example, one
Gateway process can supervise an application CDC device, a USB Serial/JTAG
device, and two network devices at the same time:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web \
  --usb /dev/serial/by-id/usb-Espressif_ESP-Iris_A... \
  --usb-serial-jtag /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_B... \
  --tcp 192.0.2.21:19772 \
  --tcp 192.0.2.22:19772
```

Automatic application CDC discovery can add further devices while the Gateway
is running. A device selects one transport in its firmware and cannot maintain
simultaneous USB and TCP sessions to the same Gateway.

### Automatic application USB discovery

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web
```

The default process discovers all visible ESP-Iris application CDC devices,
listens on `127.0.0.1:8443`, and serves the API and Workbench.

### Select an application USB port

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web \
  --usb /dev/serial/by-id/usb-Espressif_ESP-Iris_...
```

Application CDC0 carries framed ESP-Iris data, not a text console. Flash and
monitor through a separate UART/Serial-JTAG interface or manually enter the ROM
downloader when required.

### Select USB Serial/JTAG

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web \
  --usb-serial-jtag /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...
```

Automatic USB Serial/JTAG probing is intentionally disabled because its fixed
descriptor does not identify the running firmware. Use the explicit option
above or opt in with `--discover-usb-serial-jtag`. Stop the Gateway before
flashing or monitoring through the same endpoint.

### Connect raw TCP

The device application must first create its network interface. Then select its
TCP endpoint in the Gateway/Workbench. Start with the
[`tcp_wifi`](../examples/tcp_wifi/README.md) or
[`tcp_pairing`](../examples/tcp_pairing/README.md) example.

## Access and authentication

Loopback clients are authentication-free by default. Use
`--require-local-auth` to exercise the login flow locally:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web --require-local-auth
```

A new Gateway initializes the developer password to `espressif`. Override that
first-run value with `ESP_IRIS_DEVELOPER_PASSWORD` or `--password-file`, then
change it from System Settings. Do not expose the default password outside an
isolated development environment.

To serve a trusted development LAN explicitly:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web \
  --listen 0.0.0.0 --port 8443
```

Plain HTTP does not protect passwords, Agent Tokens, logs, media, or commands.
Enable `--tls` for a generated local certificate, or provide `--tls-cert` and
`--tls-key`. The Gateway prints the generated certificate SHA-256 fingerprint.

Do not rely on `X-Forwarded-For` to turn a remote client into a loopback client;
the Gateway authorizes the actual TCP peer.

## Workbench pages

| Page | Purpose |
| --- | --- |
| Overview | Device identity, connection, firmware, health, and current status |
| Logs | Live and retained device logs |
| Workspace | RPC, console, screen/input, media, firmware, OTA, and restart actions |
| Operations | Durable long-running operation state and progress |
| Records | Sessions, evidence, artifacts, and retained history |
| Settings | Access mode, credentials, tokens, TLS, and system configuration |

The interactive OpenAPI view is available at `/docs`; the machine-readable
contract is `/v1/openapi.json`; metrics are exposed at `/v1/metrics`.

## Command-line client

The `ctl` client talks to the same Gateway API as the Workbench. It can run at
the same time as one or more Workbench and agent clients:

```bash
IRIS="$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py"

python "$IRIS" ctl devices
python "$IRIS" ctl status DEVICE_ID
python "$IRIS" ctl logs --device DEVICE_ID --follow
python "$IRIS" ctl rpc DEVICE_ID system.info --params '{}'
python "$IRIS" ctl jobs DEVICE_ID JOB_ID
python "$IRIS" ctl cancel DEVICE_ID JOB_ID
python "$IRIS" ctl screenshot DEVICE_ID device.png
python "$IRIS" ctl mirror DEVICE_ID start --fps 5
python "$IRIS" ctl mirror DEVICE_ID stop
python "$IRIS" ctl restart DEVICE_ID
python "$IRIS" ctl firmware-add build/app.bin
python "$IRIS" ctl ota DEVICE_ID build/app.bin
python "$IRIS" ctl ota-status OPERATION_ID
python "$IRIS" ctl ota-watch OPERATION_ID
python "$IRIS" ctl mode observe
```

Use `ctl --json` for stable machine-readable output. A remote Gateway or local
Gateway started with `--require-local-auth` requires a login or named Agent
Token. Pass Agent Tokens through a protected file rather than a command-line
argument:

```bash
export ESP_IRIS_AGENT_TOKEN_FILE=/private/path/agent.token
python "$IRIS" ctl --json devices
```

Profiles store the Gateway URL, browser-style session, optional CA path, and
optional certificate fingerprint. Configure a profile with:

```bash
python "$IRIS" ctl --profile lab-a \
  --url https://192.0.2.10:8443 --ca gateway.crt profile --make-default
```

## Runtime and storage model

- One Gateway supervises many USB/TCP endpoints concurrently and indexes their
  sessions and events by stable `device_id`.
- Each physical device has at most one active session, even if more than one
  configured endpoint resolves to that device.
- Each WebSocket client receives its own live event queue and can resume from a
  retained event cursor. Workbench, CLI, and agents therefore observe without
  consuming one another's events.
- A stable `device_id` joins normal and recovery firmware into one device
  history; `boot_id` identifies a boot and `session_id` identifies a link.
- Device writes are serialized per device. OTA/recovery holds that device's
  write lane without blocking observation of it or other devices.
- `operation_id` makes long-running Gateway operations queryable and
  idempotent; Gateway restart does not replay device writes.
- Observe mode blocks business device requests while protocol housekeeping and
  device-pushed events continue.
- SQLite stores devices, sessions, operations, audit, token metadata, and log
  indexes. Raw logs rotate; explicitly saved artifacts and structured evidence
  remain durable.
- A newly connected Workbench, CLI follower, or agent receives recent stored
  events before live events, preserving continuity across reconnects.

## Troubleshooting

### The Workbench says the frontend is not built

Run `npm ci && npm run build` in `tools/frontend`, then restart the Gateway.

### Opening USB resets or disconnects the board

Confirm that firmware and Gateway use the same transport. For USB Serial/JTAG,
disable the ESP-IDF USB Serial/JTAG console and do not let flashing/monitoring
tools own the endpoint concurrently. For application CDC0, use a separate
programming interface.

### TCP never becomes reachable

ESP-Iris does not provision Wi-Fi or create a product network interface. Verify
that the application connected, obtained an address, and permits inbound TCP
on the configured port. Use the TCP examples as known-good references.

### Pairing fails

Verify that the device and private Gateway token store contain the same
64-character lowercase hexadecimal token. Never print the token in logs. A
device with an existing NVS token ignores a newly supplied example default
until it is explicitly rotated or reprovisioned.

### Diagnose the local installation

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" doctor --json
```

## Development and tests

From the repository root:

```bash
python3 -m pip install -r components/esp_iris/tools/requirements-dev.txt
cd components/esp_iris/tools
python3 -m ruff check iris_gateway tests
python3 -m pytest

cd frontend
npm ci
npm run test:unit
npm run build
```

Firmware examples are under [`../examples`](../examples/README.md). Build them
from the repository root with paths such as:

```bash
idf.py -C components/esp_iris/examples/minimal -B build-ci-tcp build
idf.py -C components/esp_iris/examples/rpc_jobs -B build-ci build
```

See the [component README](../README.md), [Chinese README](../README_zh.md), and
[wire protocol](../protocol/spec.md) for the device-side integration contract.
