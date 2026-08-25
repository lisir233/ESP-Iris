# ESP-Iris OTA example

This USB example provides repeatable recovery-first and application-direct OTA
test targets with no BSP or GSP dependency. It includes a retained factory
recovery image, a 16 MB dual-slot partition table, bootloader rollback,
recovery metadata hooks, automatic health acceptance, and diagnostic RPCs.

| RPC | ID | Behavior |
| --- | --- | --- |
| OTA state | `0x1200/1` | Returns JSON with version, running/next slot and recovery metadata |
| Accept image | `0x1200/2` | Calls `esp_iris_mark_healthy()` and cancels rollback |
| Enter recovery | `0x7fff/2` | Selects the factory image and performs a planned restart |

## Select the execution path

The committed A/B and rollback profiles use recovery-first OTA:

```text
normal APP:  CONFIG_ESP_IRIS_OTA=n
             CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY=y
recovery:    CONFIG_ESP_IRIS_OTA=y
```

The normal APP therefore has no Iris OTA channel, image receiver, SHA pipeline
or flash writer. It retains only RPC `0x7fff/2`, planned-restart metadata and
post-boot health acceptance. Factory recovery owns the update transfer.

The alternative `sdkconfig.application.defaults` profile switches to direct
application OTA:

```text
normal APP:  CONFIG_ESP_IRIS_OTA=y
             CONFIG_ESP_IRIS_OTA_EXAMPLE_DIRECT_APPLICATION=y
             CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY=n
```

Firmware policy and the Gateway request must agree. Recovery-first is the
Gateway default; use `--execution-mode application` only with the direct
profile. There is no silent fallback between the two paths.

## Install recovery and build A/B

From this directory, build and initially flash the recovery profile through a
UART or Serial/JTAG programming port. This one-time full flash installs the
factory recovery image, dual-slot partition table and rollback-enabled
bootloader:

```bash
idf.py -B build-recovery \
  -D SDKCONFIG_DEFAULTS=sdkconfig.recovery.defaults build
idf.py -B build-recovery -p /dev/serial/by-id/<programming-port> flash
```

Build A and B in separate directories:

```bash
idf.py -B build-a build
idf.py -B build-b \
  -D SDKCONFIG_DEFAULTS=sdkconfig.candidate.defaults build
```

Connect the Gateway to the recovery USB CDC port and OTA A. Because recovery is
already running, the Gateway writes A directly to an OTA slot. Once A is
healthy, OTA B with the same default recovery execution mode. The Gateway asks
A to enter factory recovery, waits for the same device ID to enumerate in
recovery mode, and only then transfers B. Recovery avoids the retained
last-known-good slot. B marks itself healthy after three seconds, letting the
Gateway close the reconnect/acceptance loop.

```bash
python3 components/esp_iris/tools/esp_iris.py ctl ota \
  DEVICE_ID examples/esp_iris_ota/build-a/esp_iris_ota.bin --wait
python3 components/esp_iris/tools/esp_iris.py ctl ota \
  DEVICE_ID examples/esp_iris_ota/build-b/esp_iris_ota.bin --wait
```

To build and test the alternative direct writer:

```bash
idf.py -B build-application \
  -D SDKCONFIG_DEFAULTS=sdkconfig.application.defaults build
python3 components/esp_iris/tools/esp_iris.py ctl ota \
  DEVICE_ID examples/esp_iris_ota/build-application/esp_iris_ota.bin \
  --execution-mode application --wait
```

To test rollback, build the non-accepting profile and OTA it instead:

```bash
idf.py -B build-rollback \
  -D SDKCONFIG_DEFAULTS=sdkconfig.rollback.defaults build
```

The rollback profile stays pending until RPC `0x1200/2` accepts it. Rebooting
an unaccepted image exercises the bootloader rollback path.

Application OTA transfers update only an app slot; they do not replace the
partition table, factory recovery image, or bootloader. Do not OTA this
standalone image onto an unrelated product unless its existing flash size, OTA
slots, rollback policy, USB transport, and recovery procedure have been
verified first.
