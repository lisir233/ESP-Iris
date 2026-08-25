# ESP-Iris TCP pairing

This example demonstrates the intended ownership boundary: ESP-Iris starts a
raw TCP listener with challenge-HMAC authentication, while the application
creates the ESP-IDF Wi-Fi STA interface, connects it, and obtains an address.
It depends only on ESP-IDF and the local `esp_iris` component—no BSP or GSP is
required.

The initial pairing token is a per-device provisioning secret. It is written
only when Iris has no token in NVS; later boots preserve the stored token, so
`esp_iris_pairing_token_rotate()` remains effective. Neither the token nor the
Wi-Fi credentials are logged.

## Private local configuration

Create `sdkconfig.local.defaults` beside this README. This file must remain
untracked and contain values local to the test device:

```text
CONFIG_ESP_IRIS_EXAMPLE_WIFI_SSID="your-ssid"
CONFIG_ESP_IRIS_EXAMPLE_WIFI_PASSWORD="your-password"
CONFIG_ESP_IRIS_EXAMPLE_PAIRING_TOKEN="64-lowercase-hex-characters"
```

Generate a token without copying it into shell history, for example with a
password manager or another cryptographically secure secret generator. Store
the same token in the Gateway's private device-token store. A generated
`sdkconfig` can retain these values, so do not publish or commit it.

`CMakeLists.txt` automatically appends `sdkconfig.local.defaults` when the file
exists. On a device that already has an Iris token in NVS, the local token is
ignored. Erase NVS only when intentionally reprovisioning that device.

## Build and run

From an initialized ESP-IDF 5.5 or newer shell:

```bash
idf.py build
idf.py -p /dev/serial/by-id/<device> flash
```

Monitor over the board's Serial/JTAG or UART interface. A successful startup
logs only the assigned IP address and TCP port. Connect the Gateway to
`<assigned-ip>:19772` using the privately stored pairing token. Authentication
failures are delayed by `CONFIG_ESP_IRIS_AUTH_FAILURE_DELAY_MS`.
