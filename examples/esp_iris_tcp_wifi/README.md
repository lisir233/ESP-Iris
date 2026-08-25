# ESP-Iris TCP over Wi-Fi example

This example shows the intended ownership boundary for the TCP transport. Iris
starts a raw TCP server on port 19772, while the application initializes an
ESP-IDF Wi-Fi STA, connects it to an access point, and obtains an address by
DHCP. It depends only on ESP-IDF and the repository's local `esp_iris`
component; no product BSP or GSP is required.

## Configure credentials privately

The committed Kconfig defaults intentionally contain no credentials. Create
`examples/esp_iris_tcp_wifi/sdkconfig.local.defaults` locally:

```text
CONFIG_ESP_IRIS_EXAMPLE_WIFI_SSID="your-ssid"
CONFIG_ESP_IRIS_EXAMPLE_WIFI_PASSWORD="your-password"
```

This optional file is appended automatically by the project CMake file. It is
local secret configuration and must not be committed. Alternatively, run
`idf.py menuconfig` and set the values under **ESP-Iris TCP Wi-Fi example**;
the generated `sdkconfig` is also ignored.

## Build and run

From an initialized ESP-IDF shell:

```bash
idf.py -C examples/esp_iris_tcp_wifi -B build-tcp-wifi build
idf.py -C examples/esp_iris_tcp_wifi -B build-tcp-wifi \
  -p /dev/serial/by-id/<device> flash
```

Use a stable serial path and verify the board before flashing. The example
starts Iris before Wi-Fi to demonstrate that the TCP listener becomes
reachable when the application later creates its interface. After DHCP
succeeds, the log prints the device IP and endpoint without exposing the SSID
or password:

```text
Wi-Fi ready: ip=192.0.2.10 iris=tcp://192.0.2.10:19772
```

Periodic status logs show link/session state and frame counters. Pairing is
disabled in this basic connectivity example; use the dedicated TCP pairing
example when authentication is required.
