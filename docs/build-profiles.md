# Dependency profiles (IRIS-A02)

Set `ESP_IRIS_BUILD_PROFILE` before ESP-IDF's `project.cmake` (or pass it with
`idf.py -D ESP_IRIS_BUILD_PROFILE=...`). The default `full` preserves all current
transport choices. `usb`, `usj` and `tcp` include only that transport's dependency
set; Kconfig must enable only the matching transport. `disabled` requires
`CONFIG_ESP_IRIS_ENABLE=n` and builds the public API stubs with `esp_common` only.
Conflicting settings fail configuration rather than producing missing symbols.

ESP-IDF expands component requirements before loading Kconfig, so dependency
selection deliberately uses a CMake profile, not `CONFIG_*`. The managed TinyUSB
and mDNS packages remain resolved/downloaded by the manifest; `require: no`
lets the explicit CMake dependency profile decide whether Iris links them.
This does not remove an application's own dependency on either package.

Enabled service profiles still require app metadata, partition, NVS and crypto
support. The generic service implementation uses those APIs even when individual
write capabilities are disabled. No claim is made that selecting `usb` eliminates
all ESP-IDF networking components from an application that independently uses
Wi-Fi. `test_apps/disabled` verifies the disabled link/API contract; the regular
transport and Recovery builds verify the enabled profiles.
