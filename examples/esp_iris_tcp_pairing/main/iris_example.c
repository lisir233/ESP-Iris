#include "iris_example.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "iris_pairing";
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

void iris_example_provision_pairing(void)
{
    ESP_ERROR_CHECK(provision_initial_pairing_token());
}

void iris_example_start(void)
{
    ESP_ERROR_CHECK(esp_iris_start());
}

void iris_example_mark_services_ready(void)
{
    ESP_ERROR_CHECK(esp_iris_mark_services_ready());
}
