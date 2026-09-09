# Hardware-derived Device ID migration

ESP-Iris now derives its 16-byte Device ID from the immutable factory eFuse
Base MAC:

```text
45 53 50 2d 49 52 49 53 01 00 || factory_mac[6]
```

The same chip therefore reports the same Device ID in normal firmware and
Recovery, independent of NVS contents, USB port names, reboot count, and host.
`hardware_mac` is also exposed separately as the conventional lower-case,
colon-delimited MAC string. Boot ID remains random per boot and Session ID
remains connection-scoped.

## Compatibility and rollout

Older firmware generated a random Device ID once and stored it in NVS. The
first boot of firmware using the hardware-derived scheme intentionally changes
that legacy Device ID. Gateway keeps the old record and operation history under
the old ID as offline evidence; it does not silently merge historical records
because a legacy HELLO did not prove the hardware MAC. Refresh scripts,
bookmarks, and `--device-id` selectors from live discovery after the upgrade.
The TCP pairing token and crash-loop state remain in NVS and are not reset by
this identity migration.

Upgrade retained Recovery and normal application firmware as one rollout. A
mixed deployment can still expose the legacy ID in one mode and the derived ID
in the other. For a device already in ROM download mode, select it by
`hardware_mac`; the host reads that value directly with the ROM loader and can
then verify the derived identity after Recovery reconnects.

New hosts remain compatible with legacy ESP-Iris firmware: a HELLO without the
additive `HARDWARE_MAC` field is accepted. When the field is present, Gateway
requires the Device ID to match it and rejects duplicate physical MAC
associations.
