#include "esp_iris.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAXIMUM_RETRIES 10

static const char *TAG = "iris_pairing";
static EventGroupHandle_t s_wifi_events;
static unsigned int s_retry_count;
static const char s_initial_pairing_token[] =
    CONFIG_ESP_IRIS_EXAMPLE_PAIRING_TOKEN;

static bool token_is_valid(const char token[65])
{
    for (size_t i = 0; i < 64; ++i) {
        const char value = token[i];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return token[64] == '\0';
}

static esp_err_t pairing_token_exists(bool *exists)
{
    nvs_handle_t handle;
    *exists = false;
    esp_err_t err = nvs_open("esp_iris", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open Iris NVS");
    uint8_t token[32];
    size_t token_size = sizeof(token);
    err = nvs_get_blob(handle, "pair_token", token, &token_size);
    memset(token, 0, sizeof(token));
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read pairing state");
    if (token_size != 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    *exists = true;
    return ESP_OK;
}

static esp_err_t provision_initial_pairing_token(void)
{
    bool exists = false;
    ESP_RETURN_ON_ERROR(pairing_token_exists(&exists), TAG,
                        "check pairing state");
    if (exists) {
        ESP_LOGI(TAG, "using the pairing token already stored in NVS");
        return ESP_OK;
    }

    /* sizeof() checks the Kconfig literal without scanning beyond it. Copying
     * into a fixed buffer also satisfies the public API's 65-byte contract. */
    if (sizeof(s_initial_pairing_token) != 65) {
        ESP_LOGE(TAG, "an initial 64-hex pairing token is required");
        return ESP_ERR_INVALID_SIZE;
    }
    char token[65];
    strlcpy(token, s_initial_pairing_token, sizeof(token));
    if (!token_is_valid(token)) {
        memset(token, 0, sizeof(token));
        ESP_LOGE(TAG, "the initial pairing token must be lowercase hex");
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = esp_iris_pairing_token_set(token);
    memset(token, 0, sizeof(token));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "initial pairing token stored in NVS");
    }
    return err;
}

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
    ESP_ERROR_CHECK(provision_initial_pairing_token());

    /* Iris can bind INADDR_ANY before a network interface exists. The host
     * product remains solely responsible for creating and connecting Wi-Fi. */
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_ERROR_CHECK(wifi_start());
    ESP_ERROR_CHECK(esp_iris_mark_services_ready());
    ESP_LOGI(TAG, "ESP-Iris TCP pairing example ready");
}
