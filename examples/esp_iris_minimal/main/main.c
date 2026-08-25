#include <inttypes.h>
#include <stdio.h>

#include "esp_iris.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_ERROR_CHECK(esp_iris_mark_services_ready());
    printf("ESP-Iris minimal example started\n");

    while (true) {
        printf("uptime_us=%" PRIi64 "\n", esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
