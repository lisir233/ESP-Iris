# ESP-Iris Developer Gateway

This directory is the canonical PC implementation for ESP-Iris. It is source
inside the component, not an installable Python package. The same code runs on
Linux, macOS, and Windows with Python 3.11 or newer. Current real-board
validation is performed on Linux.

## Install runtime dependencies

Create a virtual environment anywhere outside or inside the checkout, then
install the component requirements:

```bash
python3 -m venv .venv
. .venv/bin/activate                 # Linux/macOS
python -m pip install -r common_components/esp_iris/tools/requirements.txt
```

PowerShell activation is `.venv\Scripts\Activate.ps1`. Commands below use
`python3`; on Windows use `python` if that is the Python 3.11+ launcher.

## Start the gateway and Web workbench

The one `web` process owns USB, the `/v1` gateway, and the offline frontend.
HTTP, USB hotplug discovery, `127.0.0.1:8443`, global `develop` mode, and
authentication-free loopback access are the defaults. Requests whose actual
TCP peer is not loopback still require a developer password or named Agent
Token; `X-Forwarded-For` is never trusted for this decision.

```bash
python3 common_components/esp_iris/tools/esp_iris.py web
```

Use `--require-local-auth` to require the existing authentication flow on
loopback too. A fresh Gateway initializes the developer password to
`espressif`, and the login page pre-fills it. Override the first-run value with
`ESP_IRIS_DEVELOPER_PASSWORD` or `--password-file`, then change it from System
Settings for any network that is not an isolated development LAN. The password
is hashed in the gateway state database.

Open `http://127.0.0.1:8443/`. Local access does not show a login page unless
`--require-local-auth` was used. Use `/docs` for the interactive OpenAPI view
and `/v1/openapi.json` for the JSON contract. `/v1/metrics` exposes the
dependency-free metrics snapshot; WebSocket events use the versioned
`esp-iris-event/v1` envelope and include correlation identifiers when they are
available.

To open the page from a Windows PC on the trusted local network, explicitly
bind the Linux host to the LAN and use its IP address:

```bash
python3 common_components/esp_iris/tools/esp_iris.py web \
  --listen 0.0.0.0 --port 8443
```

Then browse to `http://LINUX_LAN_IP:8443/`. The `/docs` page and
`/v1/openapi.json` follow the same local/remote authentication rule. HTTP does
not encrypt the developer password, Agent token, logs, screen data, or device
commands, so use it only on a trusted local network. Enable HTTPS with `--tls`
to generate a local certificate, or provide an external certificate with
`--tls-cert` and `--tls-key`; the gateway prints the generated certificate's
SHA-256 fingerprint.

For a fixed device port instead of automatic discovery:

```bash
python3 common_components/esp_iris/tools/esp_iris.py web \
  --usb /dev/serial/by-id/usb-Espressif_ESP-Iris_Normal_...
```

## Demo and diagnostics

Demo mode never opens USB. It runs three clearly labeled virtual devices,
including normal/recovery firmware, logs, RPC/jobs, mirror/input, OTA/restart,
disconnect and crash evidence scenarios.

```bash
python3 common_components/esp_iris/tools/esp_iris.py web --demo
python3 common_components/esp_iris/tools/esp_iris.py doctor --json
```

## Gateway-only CLI

Developers and Agents use the same REST/WebSocket contract. Local loopback CLI
calls need no credential by default. For a remote gateway, login saves a
browser-style session in the selected CLI profile:

```bash
python3 common_components/esp_iris/tools/esp_iris.py ctl login
python3 common_components/esp_iris/tools/esp_iris.py ctl devices
python3 common_components/esp_iris/tools/esp_iris.py ctl status DEVICE_ID
python3 common_components/esp_iris/tools/esp_iris.py ctl logs --device DEVICE_ID --follow
python3 common_components/esp_iris/tools/esp_iris.py ctl rpc DEVICE_ID system.info --params '{}'
python3 common_components/esp_iris/tools/esp_iris.py ctl rpc-raw DEVICE_ID 1 2 --payload '{}'
python3 common_components/esp_iris/tools/esp_iris.py ctl console DEVICE_ID help
python3 common_components/esp_iris/tools/esp_iris.py ctl console DEVICE_ID iris_info
python3 common_components/esp_iris/tools/esp_iris.py ctl jobs DEVICE_ID 42
python3 common_components/esp_iris/tools/esp_iris.py ctl cancel DEVICE_ID 42
python3 common_components/esp_iris/tools/esp_iris.py ctl screenshot DEVICE_ID device.png
python3 common_components/esp_iris/tools/esp_iris.py ctl mirror DEVICE_ID start --fps 5
python3 common_components/esp_iris/tools/esp_iris.py ctl mirror DEVICE_ID stop
python3 common_components/esp_iris/tools/esp_iris.py ctl restart DEVICE_ID
python3 common_components/esp_iris/tools/esp_iris.py ctl factory DEVICE_ID
python3 common_components/esp_iris/tools/esp_iris.py ctl firmware-add build/app.bin
python3 common_components/esp_iris/tools/esp_iris.py ctl ota DEVICE_ID build/app.bin
python3 common_components/esp_iris/tools/esp_iris.py ctl ota-status OPERATION_ID
python3 common_components/esp_iris/tools/esp_iris.py ctl ota-watch OPERATION_ID
python3 common_components/esp_iris/tools/esp_iris.py ctl mode observe
```

Agents must use stable JSON output. A named token is required for a remote
gateway or when `--require-local-auth` is active; never pass the token as a
command argument:

```bash
export ESP_IRIS_AGENT_TOKEN_FILE=/private/path/codex-bench-a.token
python3 common_components/esp_iris/tools/esp_iris.py ctl --json devices
```

Profiles store the URL, browser session, and optional CA path or TLS
fingerprint. Use `ctl --profile lab-a --url http://192.168.1.20:8443 profile
--make-default` to configure the default LAN HTTP endpoint. For HTTPS, use an
`https://` URL plus `--ca gateway.crt` or `--fingerprint`.

Screenshots are normalized to real PNG/JPEG bytes at the Gateway, including
RGB565/RGB888 screen backends. The Web workbench decodes raw screen-mirror
scanline tiles onto a canvas instead of treating each tile as an encoded image.
The screenshot path assembles the next complete frame from those same tiles. If
screen mirroring is already active, the Gateway reuses it and leaves it active;
otherwise it starts and stops a temporary mirror around one frame. Repeated
mirror-start requests reuse the current stream, and the Web workbench adopts
that running stream after a reused screenshot instead of exposing a stale local
state. Screen media is centered in a 360 px-high surface without upscaling past
its intrinsic dimensions. The USB reader consumes one blocking byte and then
drains the available serial buffer in bulk; independent read/write locks let
requests transmit immediately instead of waiting for that blocking read's
200 ms timeout.

## Runtime model

- One stable `device_id` joins normal and recovery enumeration into one history.
- Per-device writes are serialized; OTA/recovery holds the write lane.
- `operation_id` is idempotent. Gateway restart never replays a write.
- Observe mode blocks every business device request. Protocol housekeeping
  and device-pushed logs/events continue; status is explicitly marked stale.
- Device operations and system audit are separate. Code edits, builds and PC
  shell activity are never added to the device timeline.
- SQLite stores devices, sessions, operations, audit, token metadata and log
  indexes. Compressed raw logs rotate at 7 days or 1 GiB. Structured evidence
  and explicitly saved artifacts do not expire automatically.
- SQLite schema changes are ordered migrations recorded in `PRAGMA
  user_version`. Startup upgrades older databases transactionally and rejects
  a database created by a newer Gateway instead of guessing at compatibility.
- A new Web workbench connection receives the newest 3000 stored events before
  switching to live delivery, so older retained history cannot hide current
  device logs.
- The normal template's line Console sends one bounded UTF-8 command through a
  cataloged RPC, executes it serially on a dedicated device task, represents it
  as an Iris Job, and returns output through the existing LOG stream. CDC0
  remains exclusively framed ESP-Iris traffic and is never a raw text console.
- OTA archives BIN, ELF and map together under the verified ELF SHA, validates
  the ESP image structure, ESP32-S31 chip ID, project, version and binary SHA,
  then immediately returns a durable operation ID. Recovery execution is the
  default; application execution requires `--execution-mode application` and
  recovery failure never silently falls back. `ota-status` and `ota-watch`
  expose stages, device Job ID, byte counts and permille progress through both
  recovery and normal-firmware reboots. Completion requires same-device
  reconnect, a new boot ID, expected project/version and healthy acceptance.
  Crash evidence is collected independently and is not attributed to OTA.

The repository-wide quality gates have one local entry point:

```bash
python3 -m pip install -r common_components/esp_iris/tools/requirements-dev.txt
python3 tools/ci.py all
python3 tools/check_esp_iris_budgets.py --require-build-artifacts
```

The same commands back `.gitlab-ci.yml`. Python module and contract tests,
frontend unit/build/audit checks, catalog consistency, and source/artifact size
budgets fail closed. Firmware compilation still requires the ESP-IDF runner.
The committed `frontend/dist/` is the runtime asset; Node is not required on
developer gateway hosts. See `docs/esp-iris-architecture.md` for layer
ownership, dependency rules, state machines, testing strategy, observability,
and migration policy.
