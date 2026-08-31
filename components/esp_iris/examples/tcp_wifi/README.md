# Raw TCP over application-owned Wi-Fi

This example demonstrates the TCP ownership boundary. ESP-Iris starts a raw
server on port `19772`; the application initializes an ESP-IDF Wi-Fi station,
connects to an access point, obtains an address through DHCP, and reconnects
when needed. The application also owns the mDNS daemon and hostname, then asks
ESP-Iris to publish `_esp-iris._tcp.local.` after DHCP succeeds.

Pairing is disabled here so the example focuses on networking. Use
[`tcp_pairing`](../tcp_pairing/README.md) when authentication is required.

## Configure credentials

Create `sdkconfig.local.defaults` in this directory:

```text
CONFIG_ESP_IRIS_EXAMPLE_WIFI_SSID="your-ssid"
CONFIG_ESP_IRIS_EXAMPLE_WIFI_PASSWORD="your-password"
```

The project appends this ignored file automatically. Alternatively use
`idf.py menuconfig` under **ESP-Iris TCP Wi-Fi example**. Do not commit the
generated `sdkconfig`, which also contains the credentials.

## Build and flash

```bash
idf.py -C components/esp_iris/examples/tcp_wifi \
  -B build-tcp-wifi build

idf.py -C components/esp_iris/examples/tcp_wifi \
  -B build-tcp-wifi \
  -p /dev/serial/by-id/<programming-port> flash
```

## Expected result

ESP-Iris starts before Wi-Fi, then becomes reachable after DHCP succeeds. The
device log prints the assigned IP and endpoint without printing the SSID or
password:

```text
Wi-Fi ready: ip=192.0.2.10 iris=tcp://192.0.2.10:19772
```

The Gateway discovers that endpoint automatically. Its service instance is
`ESP-Iris-<last three STA MAC bytes>` so several boards can advertise the same
service type without colliding. Confirm that status/log traffic continues
across a Wi-Fi reconnect. Use `--no-discover-mdns` to test the manual `--tcp`
path instead.

Return to the [example index](../README.md).
