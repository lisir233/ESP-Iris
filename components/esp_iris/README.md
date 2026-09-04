# ESP-Iris

[中文说明](README_zh.md)

ESP-Iris reduces time lost to unnecessary build-and-flash cycles in embedded
development. It makes device logs, state, and operation results easier for
developers and AI agents to understand, control, and trace at the same time.
It turns scattered serial debugging into a unified, structured, and resumable
device-development workflow.

ESP-Iris moves the debug and control Web plane to the PC. Each ESP32 runs a
bounded binary link with one worker task and exposes logs, status, and optional
RPC, media, crash, pairing, OTA, and bounded file services over USB or TCP. It does not run
an HTTP server, WebSocket server, JSON parser, framebuffer mirror, or
allocate media-sized buffers.

Multiple devices can connect concurrently to one Python Developer Gateway
through independent USB or TCP endpoints. The Gateway identifies each device
by its stable `device_id`, aggregates and persists fleet events, and fans them
out to the React Web Workbench, command-line clients, and external agents at
the same time.

Use ESP-Iris when a product needs a browser-based engineering interface without
embedding a Web server or debug UI in its firmware.

```text
ESP32 A -- USB CDC0 ---------\
ESP32 B -- USB Serial/JTAG ---+--> Python Gateway + storage --+--> Web Workbench
ESP32 C -- TCP/Wi-Fi --------/                              +--> CLI clients
ESP32 D -- TCP/Ethernet -----/                              +--> External agents
```

Each firmware image selects one device transport, and each physical device has
at most one active Gateway session. This does not limit the fleet: one Gateway
can supervise many device endpoints concurrently. Workbench, CLI, and agent
connections receive independent event streams; device-changing operations are
serialized per device.

## What you get

| Layer | Included capability |
| --- | --- |
| Device component | One bounded worker, binary framing, logs, status, RPC/jobs, media, crash evidence, pairing, and OTA |
| Developer Gateway | Concurrent USB/TCP endpoint supervision, fleet session management, authentication, durable operations, artifacts, and REST/WebSocket APIs |
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
| Gateway | Python 3.8 or newer; Linux is the primary real-device validation platform |
| Workbench | A current Node.js/npm environment is required to build the bundled React source |

One or more device transports may be compiled into an image. When several are
enabled, physical connections negotiate in bounded turns and the first valid,
authenticated-when-required HELLO_ACK owns the single active session. Losing
transports stop until the winner disconnects, then all configured transports
wait again. A port open by itself never claims the device.

## Quick start

### 1. Add the component

Add ESP-Iris to your application's `main/idf_component.yml`:

```yaml
dependencies:
  lisir233/esp_iris: "^0.1.0"
```

Run `idf.py reconfigure` after adding or changing managed dependencies.

### 2. Select transports

Open `idf.py menuconfig`, then go to:

```text
Component config > ESP-Iris device link > Device transports
```

- **Raw TCP** listens on port `19772` by default. Your application must create
  and maintain the network interface.
- **USB CDC0** owns the application TinyUSB CDC port. It is an ESP-Iris binary
  endpoint, not a text console or automatic flashing port.
- **USB Serial/JTAG** owns the fixed serial channel while preserving JTAG. Do
  not open the same serial endpoint from the Gateway and a flashing/monitoring
  tool at the same time.

Select any compatible combination. With multiple transports, provisional
connections have a configurable handshake timeout; `esp_iris_status_t` reports
transport `NONE` while no candidate is negotiating or active.

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
python -m pip install -r "$ESP_IRIS_COMPONENT_DIR/tools/requirements.lock"

cd "$ESP_IRIS_COMPONENT_DIR/tools/frontend"
npm ci
npm run build
cd -

python "$ESP_IRIS_COMPONENT_DIR/tools/esp_iris.py" web
```

The single lock file uses Python-version markers, so pip selects the validated
dependency set for the active interpreter. Recreate the environment when its
Python major/minor changes. This Gateway runtime is independent of the Python
version required by the selected ESP-IDF release; keep the ESP-IDF tool
environment on the version required by that release.

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
| stdout/stderr logs | Enabled; 8 KiB ring in PSRAM when available | `CONFIG_ESP_IRIS_LOG_RING_BYTES` and `CONFIG_ESP_IRIS_LOG_RING_STORAGE_*` |
| RPC handlers | Registered by the application | `CONFIG_ESP_IRIS_MAX_RPC_HANDLERS` and `CONFIG_ESP_IRIS_RPC_BODY_BYTES` |
| Retained jobs | Created by the application | `CONFIG_ESP_IRIS_MAX_JOBS` |
| Screen/image/audio | Idle until the host starts a stream | One `CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES` buffer per active channel |
| Crash evidence | Read-only when present | Chunked by `CONFIG_ESP_IRIS_CRASH_CHUNK_BYTES` |
| TCP pairing | Disabled by default | One token in `CONFIG_ESP_IRIS_NVS_PARTITION_NAME` and challenge-HMAC state |
| OTA writer | Configurable; cross-project updates allowed by default | Chunked by `CONFIG_ESP_IRIS_OTA_CHUNK_BYTES`; `CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH` opts into matching the running project |
| System inventory | Disabled until a read-only product provider registers | Actual protected-region hashes and last committed operation; no write callbacks |
| System Update | Disabled until recovery registers a product backend | Optional product signature policy, `CONFIG_ESP_IRIS_SYSTEM_UPDATE_MAX_COMPONENTS`, bounded manifest/signature/chunk sizes, and no generic raw-Flash API |
| File service | Disabled until the application registers a logical volume | One file task, one stream, and `CONFIG_ESP_IRIS_FILE_CHUNK_BYTES` per chunk |

The component uses credit-based channels and a latest-chunk policy for media.
A slow host cannot create an unbounded device-side queue.

The stdout/stderr ring is also mapped into a Core Dump memory section. Sending
a record to the host does not remove its bytes from the retained crash history;
complete records remain until newer records overwrite them. The default storage
is PSRAM when external BSS placement is enabled, with internal DRAM available
for products that prefer stronger crash-time reliability. The Core Dump symbol
`g_iris_log_storage` contains a self-describing header followed by the retained
record ring.

ESP-Iris stores its stable Device ID and TCP pairing token in the NVS partition
selected by `CONFIG_ESP_IRIS_NVS_PARTITION_NAME` (default: `nvs`). Products may
point this setting at a fixed system metadata partition to keep identity out of
application NVS.

Register only the directories that the product intentionally exposes, before
calling `esp_iris_start()`:

```c
ESP_ERROR_CHECK(esp_iris_file_volume_register(
    &(esp_iris_file_volume_config_t) {
        .id = "cfg",
        .base_path = "/littlefs/export",
        .capabilities = ESP_IRIS_FILE_VOLUME_READ |
                        ESP_IRIS_FILE_VOLUME_LIST |
                        ESP_IRIS_FILE_VOLUME_MTIME |
                        ESP_IRIS_FILE_VOLUME_WRITE |
                        ESP_IRIS_FILE_VOLUME_DELETE |
                        ESP_IRIS_FILE_VOLUME_MKDIR |
                        ESP_IRIS_FILE_VOLUME_RENAME |
                        ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE,
    }));
ESP_ERROR_CHECK(esp_iris_start());
```

Paths sent over the wire are relative to the registered root. Downloads and
uploads are streamed through the Gateway without buffering a complete file;
downloads support HTTP Range. Uploads use a same-directory temporary file,
strict offset ACKs, SHA-256, `fsync`, and rename. Declare `ATOMIC_REPLACE` only
when the backing VFS provides the required replacement behavior. Rename never
overwrites, delete accepts only files and empty directories, and recursive or
cross-volume operations are not exposed.

## Security model

- USB transports rely on physical access and do not perform link pairing.
- Raw TCP pairing is optional and disabled by default. When enabled, the token
  remains in NVS and the link proves possession using a fresh challenge.
- Loopback Gateway access is authentication-free by default. Non-loopback
  clients require a developer login or named Agent Token. File access by Agent
  Token is separated into `files.read`, `files.write`, and `files.delete`
  scopes; new tokens default to `files.read`.
- Plain HTTP exposes credentials and device data to the local network. Use it
  only on a trusted development LAN, or enable Gateway TLS.
- Wi-Fi credentials, pairing tokens, TLS private keys, and Agent Tokens must
  remain in ignored local configuration or private files.
- System Update is advertised only by a retained recovery image with both a
  read-only inventory provider and a registered write backend. Normal firmware
  may register only the inventory provider for post-reboot verification.
  Signed deployments configure a Gateway trust key and require the backend to
  pin the matching product trust key. Explicitly unsigned deployments omit
  both keys and rely on transport/session access control. In both modes the
  backend must protect fixed system regions and validate every component
  against the manifest plan. Inventory hashes are calculated from the actual
  protected Flash ranges, including erased-byte padding.

## TCP discovery with mDNS

Products that enable the TCP transport can advertise it through DNS-SD after
the product has initialized mDNS and selected its hostname:

```c
ESP_ERROR_CHECK(mdns_init());
ESP_ERROR_CHECK(mdns_hostname_set("my-product-a1b2c3"));
ESP_ERROR_CHECK(esp_iris_mdns_register(NULL));
```

The registration publishes `_esp-iris._tcp.local.` with a unique
`ESP-Iris-<MAC suffix>` instance. ESP-Iris owns only that service; call
`esp_iris_mdns_unregister()` before the product calls `mdns_free()`. Discovery
is local-link metadata, not authentication, and never publishes a pairing
token.

## Examples

All public examples are packaged with the component under [`examples/`](examples/README.md).

| Example | Transport | Start here when you need |
| --- | --- | --- |
| [`minimal`](examples/minimal/README.md) | TCP, USB CDC0, USB Serial/JTAG, or all three | Identity, lifecycle, status, logs, and transport arbitration |
| [`tcp_wifi`](examples/tcp_wifi/README.md) | TCP | Application-owned Wi-Fi and DHCP |
| [`tcp_pairing`](examples/tcp_pairing/README.md) | TCP | Challenge-HMAC pairing and token provisioning |
| [`rpc_jobs`](examples/rpc_jobs/README.md) | USB CDC0 | RPC handlers and cancellable jobs |
| [`display_input`](examples/display_input/README.md) | USB CDC0 | Screenshot, screen mirror, and pointer input |
| [`media_streams`](examples/media_streams/README.md) | USB CDC0 | Synthetic image and PCM audio streams |
| [`file_transfer`](examples/file_transfer/README.md) | USB CDC0 | Streamed file upload/download and metadata mutations |
| [`ota`](examples/ota/README.md) | USB CDC0 | Recovery-first/direct OTA, acceptance, and rollback |
| [`file_service`](examples/file_service/README.md) | USB CDC0 | FATFS logical volume and bounded file operations |
| [`crash_recovery`](examples/crash_recovery/README.md) | USB CDC0 | Retained Core Dump and factory recovery after repeated crashes |
| [`lifecycle`](examples/lifecycle/README.md) | USB CDC0 | Stop, unregister, restart, and reconnect |

Hardware-focused internal fixtures remain under `test_apps/`; they are not part
of the published component archive.

## Documentation map

- [Public C API](include/esp_iris.h)
- [System Inventory provider API](include/esp_iris_system_inventory.h)
- [System Update backend API](include/esp_iris_system_update.h)
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
