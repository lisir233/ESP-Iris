# Recovery-first and direct OTA

This ESP32-S31 USB CDC0 example provides repeatable OTA test images without a
product BSP or GSP. It includes a 16 MB dual-slot partition table, retained
factory recovery image, bootloader rollback, product recovery hooks, automatic
health acceptance, and diagnostic RPCs.

Do not flash or OTA these images onto an unrelated product until its flash
size, partition table, rollback policy, USB transport, and recovery procedure
have been reviewed.

## Diagnostic RPCs

| RPC | ID | Behavior |
| --- | --- | --- |
| OTA state | `0x1200/1` | Return version, running/next slot, and recovery metadata |
| Accept image | `0x1200/2` | Call `esp_iris_mark_healthy()` and cancel rollback |
| Enter recovery | `0x7fff/2` | Select the factory image and perform a planned restart |

## Execution profiles

| Profile | Defaults file | Behavior |
| --- | --- | --- |
| Factory recovery | `sdkconfig.recovery.defaults` | ESP-Iris OTA writer enabled; receives updates for normal slots |
| Normal A | `sdkconfig.defaults` | Recovery-first; normal firmware has no direct writer |
| Normal B | `sdkconfig.candidate.defaults` | Recovery-first candidate version |
| Rollback test | `sdkconfig.rollback.defaults` | Does not accept automatically |
| Direct application | `sdkconfig.application.defaults` | Writes a non-running application slot directly |

Recovery-first normal firmware sets
`CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY=y` and can omit the OTA writer. The
Gateway asks it to boot factory recovery, waits for the same `device_id` to
reconnect in recovery mode, and then transfers the image. There is no silent
fallback to direct application OTA.

## Install factory recovery

The first full flash installs the custom partition table, factory image, and
rollback-enabled bootloader:

```bash
idf.py -C components/esp_iris/examples/ota \
  -B build-ota-recovery \
  -D SDKCONFIG_DEFAULTS=sdkconfig.recovery.defaults build

idf.py -C components/esp_iris/examples/ota \
  -B build-ota-recovery \
  -p /dev/serial/by-id/<programming-port> flash
```

Use a UART or USB Serial/JTAG programming interface for this full flash.

## Build normal A and B

```bash
idf.py -C components/esp_iris/examples/ota \
  -B build-ota-a build

idf.py -C components/esp_iris/examples/ota \
  -B build-ota-b \
  -D SDKCONFIG_DEFAULTS=sdkconfig.candidate.defaults build
```

With factory recovery running, transfer A directly into a normal slot. After A
boots and is accepted, transfer B with the default recovery execution mode:

```bash
IRIS=components/esp_iris/tools/esp_iris.py

python "$IRIS" ctl ota DEVICE_ID \
  components/esp_iris/examples/ota/build-ota-a/esp_iris_ota.bin --wait

python "$IRIS" ctl ota DEVICE_ID \
  components/esp_iris/examples/ota/build-ota-b/esp_iris_ota.bin --wait
```

B marks itself healthy after three seconds, allowing the Gateway to complete
the reconnect and acceptance checks.

## Test direct application OTA

```bash
idf.py -C components/esp_iris/examples/ota \
  -B build-ota-application \
  -D SDKCONFIG_DEFAULTS=sdkconfig.application.defaults build

python "$IRIS" ctl ota DEVICE_ID \
  components/esp_iris/examples/ota/build-ota-application/esp_iris_ota.bin \
  --execution-mode application --wait
```

Direct application OTA updates only an app slot. It does not replace the
partition table, bootloader, or factory recovery image.

## Test rollback

```bash
idf.py -C components/esp_iris/examples/ota \
  -B build-ota-rollback \
  -D SDKCONFIG_DEFAULTS=sdkconfig.rollback.defaults build
```

The rollback image remains pending until RPC `0x1200/2` accepts it. Rebooting
without acceptance exercises the bootloader rollback path.

Return to the [example index](../README.md).
