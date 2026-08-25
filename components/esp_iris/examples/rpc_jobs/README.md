# RPC handlers and cancellable jobs

This USB CDC0 example registers two cataloged RPCs and one application-defined
long-running job.

| Operation | ID | Request | Response |
| --- | --- | --- | --- |
| `system.echo` | `1/1` | Arbitrary bytes | Exact request bytes |
| `system.info` | `1/2` | Ignored JSON object | JSON device/runtime summary |
| Start example job | `0x1100/1` | Empty | Four-byte little-endian job ID |

The job advances for about ten seconds. Only one example job runs at a time;
the standard Gateway job query and cancel paths exercise retained state and the
cancellation callback.

## Build and flash

```bash
idf.py -C components/esp_iris/examples/rpc_jobs \
  -B build-rpc-jobs build

idf.py -C components/esp_iris/examples/rpc_jobs \
  -B build-rpc-jobs \
  -p /dev/serial/by-id/<programming-port> flash
```

Application CDC0 is the ESP-Iris binary endpoint. Use a separate UART or USB
Serial/JTAG programming interface for flashing and monitoring.

## Exercise the example

Start the Gateway, then use the Workbench RPC tools or CLI:

```bash
IRIS=components/esp_iris/tools/esp_iris.py
python "$IRIS" ctl rpc DEVICE_ID system.echo --params '{"value":"hello"}'
python "$IRIS" ctl rpc DEVICE_ID system.info --params '{}'
python "$IRIS" ctl rpc-raw DEVICE_ID 4352 1 --payload '{}'
```

Decode the raw four-byte response as the job ID, then query or cancel it with
`ctl jobs` and `ctl cancel`. The example intentionally has no Wi-Fi, screen, or
OTA slot.

Return to the [example index](../README.md).
