# ESP-Iris RPC and Job example

This USB example demonstrates the stable RPC catalog entries and an
application-defined cancellable Job. It depends only on ESP-IDF and the local
`components/esp_iris` component; no BSP or GSP is required.

| RPC | ID | Request | Response |
| --- | --- | --- | --- |
| `system.echo` | `1/1` | Arbitrary bytes (JSON through the catalog) | Exact request bytes |
| `system.info` | `1/2` | Ignored JSON object | JSON device/runtime summary |
| Start long Job | `0x1100/1` | Empty | Four-byte little-endian Job ID |

The Job advances for about ten seconds. The Gateway's normal Job query and
cancel endpoints exercise `esp_iris_job_get_info()` and the cancellation
callback; only one example Job runs at a time.

Build from an initialized ESP-IDF shell:

```bash
idf.py -C examples/esp_iris_rpc_jobs -B build build
```

Flash through the board's Serial-JTAG/UART programming port. Application USB
CDC0 is the Iris binary link and is not a text console. After starting the
Gateway, use `system.echo` and `system.info` through the catalog, or invoke the
long task with raw RPC service `0x1100`, method `1`. Decode its four-byte
response as a little-endian Job ID, then use the Gateway `jobs` and `cancel`
commands with that ID.

This example intentionally does not initialize Wi-Fi, provide a screen, or
configure an OTA slot.
