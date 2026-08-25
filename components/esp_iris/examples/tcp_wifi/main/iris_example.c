#include "iris_example.h"

#include <inttypes.h>

#include "esp_iris.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "iris_tcp_wifi";

void iris_example_start(void)
{
    ESP_ERROR_CHECK(esp_iris_start());

    char device_id[33];
    ESP_ERROR_CHECK(esp_iris_format_device_id(device_id));
    ESP_LOGI(TAG, "Iris started before Wi-Fi: device_id=%s port=%d",
             device_id, CONFIG_ESP_IRIS_TCP_PORT);
}

void iris_example_monitor(void)
{
    while (true) {
        esp_iris_status_t status;
        ESP_ERROR_CHECK(esp_iris_get_status(&status));
        ESP_LOGI(TAG, "status link=%d session=%d uptime_us=%" PRIu64
                      " rx=%" PRIu32 " tx=%" PRIu32,
                 status.link_connected, status.session_ready,
                 status.uptime_us, status.rx_frames, status.tx_frames);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
