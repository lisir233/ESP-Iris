# ESP-Iris minimal example

The default build uses raw TCP and assumes the surrounding product starts its
own network interface. Change `app_main` or add the product Wi-Fi component
before expecting port 19772 to be reachable.

To build the USB CDC0 variant in a separate directory:

```bash
idf.py -B build-usb -D SDKCONFIG_DEFAULTS=sdkconfig.usb.defaults build
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
`common_components/esp_iris/test_apps/coredump` and is not a board flashing
profile.

The example deliberately registers no RPC or screen backend, leaves mirror
off, and has no eligible OTA slot. Those requests return deterministic
unsupported/not-found errors while the one-line baseline retains its low
resource use. The build-only feature profile above compiles RPC, screen,
pairing, dual-slot OTA, rollback and recovery hooks without changing this
device's partition table.
