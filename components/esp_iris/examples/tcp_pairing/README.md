# Authenticated raw TCP pairing

This example combines application-owned Wi-Fi with ESP-Iris challenge-HMAC
authentication. A per-device token is provisioned into NVS; each new link
proves possession using a fresh challenge without transmitting the token.

## Configure private values

Create `sdkconfig.local.defaults` in this directory:

```text
CONFIG_ESP_IRIS_EXAMPLE_WIFI_SSID="your-ssid"
CONFIG_ESP_IRIS_EXAMPLE_WIFI_PASSWORD="your-password"
CONFIG_ESP_IRIS_EXAMPLE_PAIRING_TOKEN="64-lowercase-hex-characters"
```

Generate the token with a password manager or another cryptographically secure
secret generator. Store the same value in the Gateway's private device-token
store. Never commit or log it.

The configured token is installed only when ESP-Iris has no token in NVS.
Later boots preserve the stored value so `esp_iris_pairing_token_rotate()`
remains effective. Reprovision or erase NVS only when intentionally replacing
the device identity material.

## Build and flash

```bash
idf.py -C components/esp_iris/examples/tcp_pairing \
  -B build-tcp-pairing build

idf.py -C components/esp_iris/examples/tcp_pairing \
  -B build-tcp-pairing \
  -p /dev/serial/by-id/<programming-port> flash
```

## Expected result

The device logs only its assigned address and TCP port. A Gateway configured
with the matching token connects successfully; a missing or incorrect proof is
rejected after `CONFIG_ESP_IRIS_AUTH_FAILURE_DELAY_MS`.

Return to the [example index](../README.md).
