# Minimal link, status, and log example

This is the recommended first ESP-Iris firmware project. It starts the device
component, prints the persistent device ID, and emits a status line every five
seconds with transport, lifecycle, link/session state, uptime, frame counters,
dropped log bytes, and minimum remaining worker stack.

The example registers no RPC or screen backend and has no eligible OTA slot,
so it also provides a clean baseline for resource and unsupported-operation
behavior.

## Transport profiles

| Profile | Defaults file | Notes |
| --- | --- | --- |
| Raw TCP | `sdkconfig.defaults` | Default; the example does not create Wi-Fi/Ethernet, so another application layer must provide networking |
| Application USB CDC0 | `sdkconfig.usb.defaults` | ESP32-S31; ESP-Iris owns TinyUSB CDC0 |
| USB Serial/JTAG | `sdkconfig.usj.defaults` | ESP-Iris owns the fixed serial channel while JTAG remains available |

## Build

From the repository root, build each profile in an isolated directory:

```bash
idf.py -C components/esp_iris/examples/minimal \
  -B build-minimal-tcp build

idf.py -C components/esp_iris/examples/minimal \
  -B build-minimal-usb \
  -D SDKCONFIG_DEFAULTS=sdkconfig.usb.defaults build

idf.py -C components/esp_iris/examples/minimal \
  -B build-minimal-usj \
  -D SDKCONFIG_DEFAULTS=sdkconfig.usj.defaults build
```

## Run and verify

Flash through a programming port appropriate for the board. For application
USB CDC0, the first image normally uses UART/Serial-JTAG or a manually entered
ROM downloader. Stop the Gateway before flashing through a USB Serial/JTAG
endpoint used by ESP-Iris.

After the Gateway connects, logs should contain the device ID and periodic
status lines. A local `esp_iris_stop()`/`esp_iris_start()` cycle keeps the same
`boot_id`; a real reboot produces a new one.

The 2 MB defaults intentionally contain no coredump partition. Crash metadata
remains queryable, while `core_dump_present` is false. Use the internal
`test_apps/coredump` fixture when validating Flash coredump transfer.

## Next steps

- Add networking with [`tcp_wifi`](../tcp_wifi/README.md).
- Add authenticated TCP with [`tcp_pairing`](../tcp_pairing/README.md).
- Add RPC/jobs with [`rpc_jobs`](../rpc_jobs/README.md).
- Return to the [example index](../README.md).
