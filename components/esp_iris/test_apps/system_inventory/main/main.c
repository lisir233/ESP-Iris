#include "esp_iris.h"
#include "esp_iris_system_inventory.h"

#include "esp_log.h"

static const char *TAG = "iris_inventory_fixture";

static esp_err_t get_inventory(
    esp_iris_system_inventory_t *inventory, void *user_ctx)
{
    (void)user_ctx;
    if (inventory == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *inventory = (esp_iris_system_inventory_t){
        .layout_version = 1,
    };
    return ESP_OK;
}

void app_main(void)
{
    const esp_iris_system_inventory_provider_t provider = {
        .get_inventory = get_inventory,
    };
    ESP_ERROR_CHECK(esp_iris_system_inventory_register(&provider));
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_LOGI(TAG, "IRIS_SYSTEM_INVENTORY_FIXTURE_READY read_only=1");
}
