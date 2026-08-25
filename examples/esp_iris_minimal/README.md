# ESP-Iris minimal example

This example is self-contained within this repository. It depends only on
ESP-IDF and the local `components/esp_iris` component; no product BSP or GSP
component is required.

The default build uses raw TCP and assumes the surrounding product starts its
own network interface. Change `app_main` or add the product Wi-Fi component
before expecting port 19772 to be reachable.

At startup the example prints the persistent Iris device ID. It then emits a
compact status line every five seconds with the selected transport, lifecycle,
link/session state, uptime, frame counters, dropped log bytes, and the minimum
remaining Iris task stack. These messages use the Iris LOG channel after
`esp_iris_start()`; they become visible when a Gateway session is connected.

From an initialized ESP-IDF shell, build the default TCP profile from the
repository root with an isolated build directory:

```bash
idf.py -C examples/esp_iris_minimal -B build-ci-tcp build
```

To build the USB CDC0 variant in a separate directory:

```bash
idf.py -C examples/esp_iris_minimal -B build-ci-usb \
  -D SDKCONFIG_DEFAULTS=sdkconfig.usb.defaults build
```

Application USB is an ESP-Iris binary port, not a serial monitor or automatic
flashing port. Flash the first image over UART or after manually entering ROM
download mode, then run the PC Hub against the enumerated CDC device.
The USB profile routes early boot diagnostics to the board's separate USB
Serial-JTAG interface when it is connected; stdout/stderr move to the Iris LOG
channel after `esp_iris_start()` succeeds.

The minimal example uses the 2 MB flash profile so application-only flashing
remains compatible with the 32 KiB MMU page size used by the repository's
single-app examples. A product using a larger flash profile must update its
bootloader and application together so their MMU page sizes match.

This 2 MB profile intentionally has no coredump partition. Its M5 API still
reports reset/crash metadata and returns `core_dump_present=false`. Products
using the repository `template/` layout can enable Flash coredumps and retain a
complete ELF SHA; the component's internal coredump build profile is under
`components/esp_iris/test_apps/coredump` and is not a board flashing
profile.

The example deliberately registers no RPC or screen backend, leaves mirror
off, and has no eligible OTA slot. Those requests return deterministic
unsupported/not-found errors while the baseline retains low resource use. Use
the sibling RPC/Job, display/input, media, Wi-Fi, and pairing examples for
focused feature coverage.
