#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "iris_example.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAXIMUM_RETRIES 10

static const char *TAG = "iris_pairing";
static EventGroupHandle_t s_wifi_events;
static unsigned int s_retry_count;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id,
                       void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAXIMUM_RETRIES) {
            ++s_retry_count;
            (void)esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi ready at " IPSTR ", Iris TCP port %d",
                 IP2STR(&event->ip_info.ip), CONFIG_ESP_IRIS_TCP_PORT);
        /* Normal logs are captured by Iris after start. This marker bypasses
         * that redirect so HIL can discover the endpoint without secrets. */
        esp_rom_printf("IRIS_TCP_PAIRING_READY ip=" IPSTR " port=%d\n",
                       IP2STR(&event->ip_info.ip), CONFIG_ESP_IRIS_TCP_PORT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start(void)
{
    if (CONFIG_ESP_IRIS_EXAMPLE_WIFI_SSID[0] == '\0' ||
        CONFIG_ESP_IRIS_EXAMPLE_WIFI_PASSWORD[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi credentials are not configured");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                        "create event loop");
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL),
                        TAG, "register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL),
                        TAG, "register IP event handler");

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, CONFIG_ESP_IRIS_EXAMPLE_WIFI_SSID,
            sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password,
            CONFIG_ESP_IRIS_EXAMPLE_WIFI_PASSWORD,
            sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "set Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                        "set Wi-Fi configuration");
    memset(&config, 0, sizeof(config));
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");

    const EventBits_t result = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));
    if ((result & WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }
    return (result & WIFI_FAILED_BIT) != 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

void app_main(void)
{
    /* This explicit init is needed for provisioning before Iris starts. Never
     * erase shared NVS automatically when initialization reports corruption. */
    ESP_ERROR_CHECK(nvs_flash_init());
    iris_example_provision_pairing();

    /* Iris can bind INADDR_ANY before a network interface exists. The host
     * product remains solely responsible for creating and connecting Wi-Fi. */
    iris_example_start();
    ESP_ERROR_CHECK(wifi_start());
    ESP_LOGI(TAG, "ESP-Iris TCP pairing example ready");
}
