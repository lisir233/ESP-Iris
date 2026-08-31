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
| Recovery with fallback | `sdkconfig.fallback.defaults` | Recovery remains default; explicit application execution is also allowed |
| Project-name enforcement | `sdkconfig.project-match.defaults` | Rejects images whose project name differs |

Recovery-first normal firmware sets
`CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY=y` and can omit the OTA writer. The
Gateway asks it to boot factory recovery, waits for the same `device_id` to
reconnect in recovery mode, and then transfers the image. There is no silent
fallback to direct application OTA.

The fallback profile keeps both writers available, but never changes execution
mode silently. Select its application writer explicitly:

```bash
python "$IRIS" ctl ota DEVICE_ID IMAGE.bin --elf IMAGE.elf \
  --execution-mode application --wait
```

For project-name enforcement, build and flash
`sdkconfig.project-match.defaults`, then attempt to transfer a valid ESP-IDF
application from a differently named project. The writer rejects the mismatch;
an image built from this `esp_iris_ota` project remains eligible.

## Install factory recovery

The first full flash installs the custom partition table, factory image, and
rollback-enabled bootloader:

```bash
idf.py -C components/esp_iris/examples/ota \
  -B build-ota-recovery \
  -D SDKCONFIG="$PWD/build-ota-recovery/sdkconfig" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.recovery.defaults build

idf.py -C components/esp_iris/examples/ota \
  -B build-ota-recovery \
  -p /dev/serial/by-id/<programming-port> flash
```

Use a UART or USB Serial/JTAG programming interface for this full flash.

## Build normal A and B

```bash
idf.py -C components/esp_iris/examples/ota \
  -B build-ota-a \
  -D SDKCONFIG="$PWD/build-ota-a/sdkconfig" build

idf.py -C components/esp_iris/examples/ota \
  -B build-ota-b \
  -D SDKCONFIG="$PWD/build-ota-b/sdkconfig" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.candidate.defaults build
```

With factory recovery running, transfer A directly into a normal slot. After A
boots and is accepted, transfer B with the default recovery execution mode:

```bash
IRIS=components/esp_iris/tools/esp_iris.py

python "$IRIS" ctl ota DEVICE_ID \
  build-ota-a/esp_iris_ota.bin --wait

python "$IRIS" ctl ota DEVICE_ID \
  build-ota-b/esp_iris_ota.bin --wait
```

B marks itself healthy after three seconds, allowing the Gateway to complete
the reconnect and acceptance checks.

## Test direct application OTA

```bash
idf.py -C components/esp_iris/examples/ota \
  -B build-ota-application \
  -D SDKCONFIG="$PWD/build-ota-application/sdkconfig" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.application.defaults build

python "$IRIS" ctl ota DEVICE_ID \
  build-ota-application/esp_iris_ota.bin \
  --execution-mode application --wait
```

Direct application OTA updates only an app slot. It does not replace the
partition table, bootloader, or factory recovery image.

## Test rollback

```bash
idf.py -C components/esp_iris/examples/ota \
  -B build-ota-rollback \
  -D SDKCONFIG="$PWD/build-ota-rollback/sdkconfig" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.rollback.defaults build
```

Each profile deliberately keeps its generated `sdkconfig` inside its own build
directory. Do not reuse the project-level generated `sdkconfig` across these
images, because it overrides later `SDKCONFIG_DEFAULTS` selections.

The rollback image remains pending until RPC `0x1200/2` accepts it. Rebooting
without acceptance exercises the bootloader rollback path.

Return to the [example index](../README.md).
