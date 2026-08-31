# Retained Core Dump and factory recovery

This destructive USB CDC0 example demonstrates a product recovery policy on a
16 MB layout. A normal OTA application deliberately crashes after startup. It
persists a consecutive injection count before each `abort()`. At the configured
threshold (default one), it selects the factory image before aborting, so the
factory recovery boots directly after the panic while the latest Flash Core
Dump remains available through Iris.

The bootloader rollback feature is intentionally not used: a threshold greater
than one must be allowed to restart the same application repeatedly.

## Build and install factory recovery

Verify the physical flash size and target before the first full flash:

```bash
idf.py -C components/esp_iris/examples/crash_recovery \
  -B build-crash-recovery \
  -D SDKCONFIG="$PWD/build-crash-recovery/sdkconfig" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.recovery.defaults build
idf.py -C components/esp_iris/examples/crash_recovery \
  -B build-crash-recovery -p /dev/serial/by-id/<programming-port> flash
```

Build the crashing application:

```bash
idf.py -C components/esp_iris/examples/crash_recovery \
  -B build-crash-application \
  -D SDKCONFIG="$PWD/build-crash-application/sdkconfig" \
  -D SDKCONFIG_DEFAULTS=sdkconfig.application.defaults build
```

With recovery connected, transfer `build-crash-application/esp_iris_crash_recovery.bin`
using application execution mode. After the application crashes, the same
device ID reconnects in factory recovery.

```bash
IRIS=components/esp_iris/tools/esp_iris.py
python "$IRIS" ctl ota DEVICE_ID \
  build-crash-application/esp_iris_crash_recovery.bin \
  --elf build-crash-application/esp_iris_crash_recovery.elf \
  --execution-mode application --wait
python "$IRIS" ctl crash DEVICE_ID
python "$IRIS" ctl coredump DEVICE_ID retained-core-dump.bin
python "$IRIS" ctl rpc-raw DEVICE_ID 5120 1
```

RPC `0x1400/1` returns four little-endian words: count, limit, last application
address, and flags (`bit0=injection enabled`, `bit1=planned restart`). RPC
`0x1400/2` clears the count, disables injection, and resumes the application.
RPC `0x1400/3` clears the count, re-arms injection, and retries it. Both restart
RPCs delay reboot long enough for their responses to drain.

To validate a threshold of three, set
`CONFIG_ESP_IRIS_CRASH_EXAMPLE_CRASH_LIMIT=3` in an ignored local defaults file
for both profiles. Recovery never exits automatically.

Return to the [example index](../README.md).
