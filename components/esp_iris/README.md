# ESP-Iris

[中文说明](README_zh.md)

ESP-Iris is a development link for ESP-IDF devices. A small device component
exposes logs, status, and optional RPC, media, crash, pairing, and OTA services
over USB or TCP. The Python Developer Gateway and React Web Workbench run on
the PC, keeping HTTP, WebSocket, JSON, storage, and UI processing off the
device.

Use ESP-Iris when a product needs a browser-based engineering interface without
embedding a Web server or debug UI in its firmware.

```text
ESP-IDF application          Developer PC
┌──────────────────┐        ┌─────────────────┐        ┌───────────────┐
│ ESP-Iris device  │ USB/TCP│ Python Gateway  │ HTTP/WS│ Web Workbench │
│ component        ├────────┤ + local storage ├────────┤ or CLI/Agent  │
└──────────────────┘        └─────────────────┘        └───────────────┘
```

## What you get

| Layer | Included capability |
| --- | --- |
| Device component | One bounded worker, binary framing, logs, status, RPC/jobs, media, crash evidence, pairing, and OTA |
| Developer Gateway | USB/TCP discovery, session management, authentication, durable operations, artifacts, and REST/WebSocket APIs |
| Web Workbench | Device overview, logs, RPC, screen/input, media, firmware, operations, records, and settings |
| Command-line client | Scriptable control with stable JSON output for developers and agents |

The default device configuration starts only the core link and log/status
state. RPC tables, media buffers, pairing, and OTA behavior are bounded by
Kconfig and are activated only when configured or used.

## Requirements and compatibility

| Area | Requirement |
| --- | --- |
| ESP-IDF | 5.5 or newer |
| Raw TCP | Portable default; the application owns Wi-Fi/Ethernet and addressing |
| Application USB CDC0 | ESP32-S31; ESP-Iris owns TinyUSB CDC0 |
| USB Serial/JTAG | Targets with `SOC_USB_SERIAL_JTAG_SUPPORTED`; the serial channel cannot also be the console |
| Gateway | Python 3.11 or newer; Linux is the primary real-device validation platform |
| Workbench | A current Node.js/npm environment is required to build the bundled React source |

Only one device transport is compiled into a firmware image. Transport
selection is static and cannot be changed at runtime.

## Quick start

### 1. Add the component

Add ESP-Iris to your application's `main/idf_component.yml`:

```yaml
dependencies:
  lisir233/esp_iris: "^0.1.0"
```

Run `idf.py reconfigure` after adding or changing managed dependencies.

### 2. Select a transport

Open `idf.py menuconfig`, then go to:

```text
Component config > ESP-Iris device link > Device transport
```

- **Raw TCP** listens on port `19772` by default. Your application must create
  and maintain the network interface.
- **USB CDC0** owns the application TinyUSB CDC port. It is an ESP-Iris binary
  endpoint, not a text console or automatic flashing port.
- **USB Serial/JTAG** owns the fixed serial channel while preserving JTAG. Do
  not open the same serial endpoint from the Gateway and a flashing/monitoring
  tool at the same time.

See the transport-specific notes in the [example index](examples/README.md).

### 3. Start ESP-Iris

```c
#include "esp_iris.h"

void app_main(void)
{
    ESP_ERROR_CHECK(esp_iris_start());
}
```

`esp_iris_start()` is idempotent. `esp_iris_stop()` releases the worker,
transport, VFS, and stdio ownership so the component can be started again.

### 4. Build the firmware

```bash
idf.py build
```

Start with the [minimal example](examples/minimal/README.md) if you want to
validate TCP, USB CDC0, and USB Serial/JTAG profiles before integrating ESP-Iris
into a product.

### 5. Start the Developer Gateway

In a repository checkout:

```bash
ESP_IRIS_COMPONENT_DIR=components/esp_iris
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r "$ESP_IRIS_COMPONENT_DIR/tools/requirements.txt"

cd "$ESP_IRIS_COMPONENT_DIR/tools/frontend"
npm ci
npm run build
cd -

python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web
```

For a managed installation, set `ESP_IRIS_COMPONENT_DIR` to
`managed_components/lisir233__esp_iris` instead. Open
`http://127.0.0.1:8443/` after the Gateway starts.

Use demo mode to evaluate the Gateway and Workbench without hardware:

```bash
python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web --demo
```

See the [Gateway and Workbench guide](tools/README.md) for USB/TCP selection,
authentication, TLS, CLI commands, data retention, and development workflows.

## Capabilities and resource behavior

| Capability | Default behavior | Device-side bound |
| --- | --- | --- |
| Link and status | Enabled | One worker and fixed protocol buffers |
| stdout/stderr logs | Enabled | `CONFIG_ESP_IRIS_LOG_RING_BYTES` |
| RPC handlers | Registered by the application | `CONFIG_ESP_IRIS_MAX_RPC_HANDLERS` and `CONFIG_ESP_IRIS_RPC_BODY_BYTES` |
| Retained jobs | Created by the application | `CONFIG_ESP_IRIS_MAX_JOBS` |
| Screen/image/audio | Idle until the host starts a stream | One `CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES` buffer per active channel |
| Crash evidence | Read-only when present | Chunked by `CONFIG_ESP_IRIS_CRASH_CHUNK_BYTES` |
| TCP pairing | Disabled by default | One NVS token and challenge-HMAC state |
| OTA writer | Configurable | Chunked by `CONFIG_ESP_IRIS_OTA_CHUNK_BYTES`; no full image is buffered in RAM |

The component uses credit-based channels and a latest-chunk policy for media.
A slow host cannot create an unbounded device-side queue.

## Security model

- USB transports rely on physical access and do not perform link pairing.
- Raw TCP pairing is optional and disabled by default. When enabled, the token
  remains in NVS and the link proves possession using a fresh challenge.
- Loopback Gateway access is authentication-free by default. Non-loopback
  clients require a developer login or named Agent Token.
- Plain HTTP exposes credentials and device data to the local network. Use it
  only on a trusted development LAN, or enable Gateway TLS.
- Wi-Fi credentials, pairing tokens, TLS private keys, and Agent Tokens must
  remain in ignored local configuration or private files.

## Examples

All public examples are packaged with the component under [`examples/`](examples/README.md).

| Example | Transport | Start here when you need |
| --- | --- | --- |
| [`minimal`](examples/minimal/README.md) | TCP, USB CDC0, or USB Serial/JTAG | Identity, lifecycle, status, and logs |
| [`tcp_wifi`](examples/tcp_wifi/README.md) | TCP | Application-owned Wi-Fi and DHCP |
| [`tcp_pairing`](examples/tcp_pairing/README.md) | TCP | Challenge-HMAC pairing and token provisioning |
| [`rpc_jobs`](examples/rpc_jobs/README.md) | USB CDC0 | RPC handlers and cancellable jobs |
| [`display_input`](examples/display_input/README.md) | USB CDC0 | Screenshot, screen mirror, and pointer input |
| [`media_streams`](examples/media_streams/README.md) | USB CDC0 | Synthetic image and PCM audio streams |
| [`ota`](examples/ota/README.md) | USB CDC0 | Recovery-first/direct OTA, acceptance, and rollback |

Hardware-focused internal fixtures remain under `test_apps/`; they are not part
of the published component archive.

## Documentation map

- [Public C API](include/esp_iris.h)
- [Protocol constants](include/esp_iris_protocol.h)
- [Wire protocol v1](protocol/spec.md)
- [Golden protocol vectors](protocol/golden_vectors.json)
- [Gateway and Workbench](tools/README.md)
- [Examples](examples/README.md)
- [Engineering architecture](https://github.com/lisir233/ESP-Iris/blob/master/docs/esp-iris-architecture.md)
- [Changelog](CHANGELOG.md)

## Version and license

The current component release is `0.1.0`, corresponding to Git tag `v0.1.0`.
ESP-Iris is licensed under [Apache-2.0](LICENSE).
