# ESP-Iris examples

All examples depend only on ESP-IDF and the repository-local `esp_iris`
component. None requires a product BSP or GSP.

| Priority | Example | Transport | Coverage |
| --- | --- | --- | --- |
| P0 | `esp_iris_minimal` | TCP or USB | Identity, lifecycle, status and log baseline |
| P0 | `esp_iris_tcp_wifi` | TCP | Application-owned Wi-Fi STA, DHCP and reconnect |
| P0 | `esp_iris_rpc_jobs` | USB | Canonical echo/info RPCs and cancellable long Job |
| P0 | `esp_iris_display_input` | USB | Screenshot, screen mirror and pointer RPC |
| P1 | `esp_iris_media_streams` | USB | Synthetic RGB565 image and PCM S16LE audio |
| P1 | `esp_iris_tcp_pairing` | TCP | Challenge-HMAC pairing and persistent token provisioning |
| P1 | `esp_iris_ota` | USB | Configurable recovery-first/direct OTA, A/B slots, acceptance and rollback |

TCP examples deliberately keep network ownership in the application: Iris
initializes only the global TCP/IP core needed by its socket transport, while
the example creates and reconnects Wi-Fi. Real credentials and pairing tokens
belong in an ignored `sdkconfig.local.defaults`, never in committed defaults.

USB CDC0 is the Iris binary link. Flash and monitor through a separate UART or
Serial/JTAG programming interface; an already-running Iris CDC endpoint is not
automatically an ESP ROM download port.
