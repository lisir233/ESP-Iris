# ESP-Iris M5-M11 board test

This internal application compiles the retained Flash coredump path, complete
64-character ELF SHA, TCP challenge-HMAC pairing, bounded RPC registration,
pull screenshot backend, dual OTA slots, rollback support and the M11 recovery
adapter hooks. Its 16 MB partition table preserves factory, NVS and an
832 KiB coredump partition, provides two 6 MiB OTA slots, and leaves the final
`0x210000` bytes unallocated for future product data partitions.

Wi-Fi credentials and deterministic board-test pairing tokens are private
Kconfig values with empty defaults. Set them only in the generated, ignored
`sdkconfig`; do not add them to `sdkconfig.defaults` or source. Wi-Fi uses
`WIFI_STORAGE_RAM`, so its password is not persisted to NVS.

The fixture exposes RPC methods for echo, a cancellable job, pairing-token
rotation, healthy acceptance, one-shot pending-OTA crash injection and
recovery-state inspection. The crash injection clears its NVS arm flag before
calling `abort()`, allowing the bootloader to reject the unverified OTA image
and recover to factory while retaining the Flash coredump.

At startup the fixture also validates persisted last-good/target offsets
against current app-partition starts. This preserves identity and pairing NVS
while safely resetting stale recovery offsets after a partition-layout
migration.

This is a destructive board-test profile, not a production provisioning
manifest. Before a full flash, verify the physical Flash capacity and
`build/flasher_args.json`. The expected writes are bootloader `0x2000`,
partition table `0x8000`, otadata `0xf000` and factory `0x20000`; NVS is not
part of the write list.

## Board validation

On 2026-08-23 an ESP32-S31 with physically detected 16 MB Flash booted this
layout and reported `ota_0=0x120000/6M`, `ota_1=0x720000/6M` and
`coredump=0xd20000/832K`. Each OTA slot accepted a 5.5 MiB BEGIN followed by
CANCEL, proving the old 1 MiB size boundary was removed. Normal images then
completed factory-to-ota_0 and ota_0-to-ota_1 upgrades; both pending boots
were marked healthy. A further planned restart remained on
`ota_1@0x720000`, with `last_good=0x720000`, `target=0` and zero invalid
protocol frames. The layout migration retained the device identity and
pairing token while correcting the old last-good offset.
