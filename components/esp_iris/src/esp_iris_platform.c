#include "esp_iris.h"

esp_err_t __attribute__((weak)) esp_iris_platform_mark_healthy(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t __attribute__((weak)) esp_iris_platform_mark_planned_restart(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_platform_prepare_ota(uint32_t running_address,
                                        uint32_t target_address)
    __attribute__((weak));
esp_err_t esp_iris_platform_prepare_ota(uint32_t running_address,
                                        uint32_t target_address)
{
    (void)running_address;
    (void)target_address;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_platform_select_ota_target(uint32_t default_address,
                                               uint32_t *target_address)
    __attribute__((weak));
esp_err_t esp_iris_platform_select_ota_target(uint32_t default_address,
                                               uint32_t *target_address)
{
    if (target_address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *target_address = default_address;
    return ESP_OK;
}
