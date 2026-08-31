#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "esp_iris.h"
#include "iris_example.h"
#include "mdns.h"

static const char *TAG = "iris_tcp_wifi";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started; connecting");
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%u); reconnecting",
                 event->reason);
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        esp_err_t mdns_err = esp_iris_mdns_register(NULL);
        if (mdns_err != ESP_OK) {
            ESP_LOGW(TAG, "Iris mDNS registration failed: %s",
                     esp_err_to_name(mdns_err));
        }
        ESP_LOGI(TAG, "Wi-Fi ready: ip=" IPSTR " iris=tcp://" IPSTR ":%d",
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.ip),
                 CONFIG_ESP_IRIS_TCP_PORT);
        /* Iris captures normal logging after start. Keep one deterministic,
         * non-secret UART marker for hardware-in-the-loop automation. */
        esp_rom_printf("IRIS_TCP_WIFI_READY ip=" IPSTR " port=%d\n",
                       IP2STR(&event->ip_info.ip), CONFIG_ESP_IRIS_TCP_PORT);
    }
}

static esp_err_t wifi_start(void)
{
    if (CONFIG_ESP_IRIS_EXAMPLE_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty; configure it with menuconfig or "
                      "sdkconfig.local.defaults");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                        "create default event loop");
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG,
                        "read Wi-Fi STA MAC");
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "esp-iris-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "initialize mDNS");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG,
                        "set mDNS hostname");
    ESP_LOGI(TAG, "mDNS hostname=%s.local service=_esp-iris._tcp", hostname);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event_handler, NULL),
        TAG, "register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event_handler, NULL),
        TAG, "register IP event handler");

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid,
            CONFIG_ESP_IRIS_EXAMPLE_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password,
            CONFIG_ESP_IRIS_EXAMPLE_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode =
        CONFIG_ESP_IRIS_EXAMPLE_WIFI_PASSWORD[0] == '\0'
            ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "select volatile Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "select Wi-Fi STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                        "configure Wi-Fi STA");
    return esp_wifi_start();
}

void app_main(void)
{
    /* Iris owns its TCP server, while this application owns Wi-Fi. Starting
     * Iris first demonstrates that the INADDR_ANY listener remains usable
     * when the product creates its network interface later. */
    iris_example_start();

    ESP_ERROR_CHECK(wifi_start());
    iris_example_monitor();
}
