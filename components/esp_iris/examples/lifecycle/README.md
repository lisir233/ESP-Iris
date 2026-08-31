# Lifecycle stop, unregister, and restart

This USB CDC0 example demonstrates a complete application-owned Iris lifecycle.
It starts with a small screenshot backend and RPC `0x1300/1`. Calling the RPC
queues work outside the Iris worker, lets the response drain, stops Iris,
unregisters both services, waits two seconds, re-registers them, and starts
Iris again.

## Build and flash

```bash
idf.py -C components/esp_iris/examples/lifecycle -B build-lifecycle build
idf.py -C components/esp_iris/examples/lifecycle -B build-lifecycle \
  -p /dev/serial/by-id/<programming-port> flash
```

Start the Gateway, then request a cycle:

```bash
python components/esp_iris/tools/esp_iris.py ctl rpc-raw \
  DEVICE_ID 4864 1
```

The programming console reports `RUNNING`, `STOPPED`, `UNREGISTERED`, and
`RESTARTED`. CDC0 disconnects and the Gateway discovers the same device again.
Do not run a screen capture while requesting the cycle; stopping Iris closes
active captures before the backend is unregistered.

Return to the [example index](../README.md).
