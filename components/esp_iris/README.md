# ESP-Iris

ESP-Iris moves the debug/control Web plane to the PC. The ESP32 runs a bounded
binary link with one worker task and no HTTP server, WebSocket server, JSON
parser, framebuffer mirror, or media-sized allocation.

## Device integration

Add `esp_iris` to the application component dependencies and call it once:

```c
#include "esp_iris.h"

void app_main(void)
{
    ESP_ERROR_CHECK(esp_iris_start());
    printf("application ready\n");
}
```

Configuration is entirely in Kconfig/NVS. Normal firmware compiles USB CDC0 or
raw TCP, never both. The recommended Mosaico template uses the ESP-Iris USB
Highspeed path for both normal and recovery firmware.

For USB Iris, disable `CONFIG_BSP_USB_CONSOLE` and enable
`CONFIG_ESP_IRIS_TRANSPORT_USB`. Iris owns TinyUSB CDC0, redirects stdout and
stderr into its LOG channel, and uses DTR only to delimit PC sessions. It does
not interpret RTS/DTR as reset or download commands and ignores line coding.
Initial provisioning or catastrophic recovery must therefore use a separate
Serial-JTAG/UART path or a manually entered ROM USB downloader. Factory
recovery will later provide its own USB Iris image.

TCP Iris initializes the idempotent global `esp_netif`/lwIP core before it
opens a listener. The product remains responsible for NVS, interfaces, Wi-Fi,
addressing and reconnect policy; listener creation is retried while those
interfaces come up after `esp_iris_start()`.

`esp_iris_mark_healthy()` deliberately returns `ESP_ERR_NOT_SUPPORTED` until a
product recovery adapter supplies `esp_iris_platform_mark_healthy()`. Starting
the observation link must not automatically mark unverified product firmware
healthy.

`esp_iris_stop()` reverses the worker, transport, VFS and stdio ownership. A
later start keeps the same `boot_id`, because a local component restart is not
a device reboot. Optional `esp_iris_mark_services_ready()` and recovery-backed
healthy/planned-restart markers are replayed to newly connected PC sessions.

## Current milestone

M1-M11 are implemented:

- fixed v1 COBS/CRC32 envelope and incremental resynchronization;
- stable NVS device ID, per-boot boot ID and per-link session ID;
- HELLO, PING, time synchronization, status and channel credit;
- nonblocking 4 KiB stdout/stderr ring and LOG records;
- bounded lifecycle/resource telemetry with reversible stop/start;
- single CDC0 or single-client raw TCP transport;
- Python USB hotplug/TCP supervisors, endpoint locks and reconnect backoff;
- explicit boot/link/service/health event and time semantics;
- read-only reset/crash metadata and optional chunked Flash coredump download;
- fixed-size binary RPC registration, deadlines and bounded retained jobs;
- pull-based screenshot backends with no Iris framebuffer copy;
- default-off SCREEN/IMAGE/AUDIO streaming with per-channel credit and a
  latest-chunk drop policy; SCREEN can pull scanline tiles directly from the
  registered screenshot backend without a second framebuffer;
- bounded burst transmission into enlarged USB Highspeed CDC FIFOs and
  batched host reads with independent host-side read/write lanes, reducing
  per-frame scheduler and syscall overhead without making requests wait for
  the serial read timeout;
- optional TCP challenge-HMAC pairing with a random NVS token;
- cancellable, sequential OTA with queryable device progress, full-image SHA,
  image/project/version validation, recovery target-selection/preparation hooks
  and planned restart;
- loopback-trusted, remotely authenticated cross-platform Developer Gateway,
  offline React workbench with raw RGB scanline rendering, source CLI
  screenshot/mirror controls, WebSocket events and OpenAPI contract. The
  Gateway assembles screenshots from the next complete mirror frame, reuses an
  already-running mirror without stopping it, treats repeated mirror starts as
  idempotent reuse, and converts raw RGB565/RGB888 frames to browser-safe PNG
  bytes. The workbench keeps screen media within a 360 px viewing surface and
  does not upscale screenshots beyond their intrinsic resolution.

The one-line default still allocates none of the RPC/media state. Registering
an RPC or screen backend creates one bounded service table. Starting a media
channel allocates one `CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES` buffer and stopping
it releases that buffer. Its USB Highspeed-oriented default is 3840 bytes;
products may lower it when RAM is tighter. OTA state exists only between BEGIN
and END/CANCEL.

For products with a recovery partition contract, override
`esp_iris_platform_select_ota_target()`,
`esp_iris_platform_prepare_ota()`,
`esp_iris_platform_mark_planned_restart()` and
`esp_iris_platform_mark_healthy()`. Target selection prevents a factory
recovery writer from overwriting the retained last-known-good normal slot;
prepare records last-known-good and target offsets before Iris changes the
boot partition. Its default `ESP_ERR_NOT_SUPPORTED` result prevents boot-slot
selection. Healthy must only succeed after product acceptance, and marking the
factory recovery image healthy must not promote factory to last-known-good.

TCP pairing is disabled by default. When enabled, retrieve or rotate the token
through a product-owned secure provisioning surface using
`esp_iris_pairing_token_get()`/`esp_iris_pairing_token_rotate()`, or install a
pre-provisioned 64-hex token with `esp_iris_pairing_token_set()`. Iris never
prints or transports provisioning tokens, and USB remains
authentication-free.

See [protocol/spec.md](protocol/spec.md) for the wire contract and
[tools/README.md](tools/README.md) for the component-contained PC gateway.
The repository-wide layer ownership, state-machine, test, observability,
migration and resource-budget rules are in
[docs/esp-iris-architecture.md](../../docs/esp-iris-architecture.md).
