#include <inttypes.h>
#include <stdio.h>

#include "esp_iris.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *transport_name(esp_iris_transport_kind_t transport)
{
    switch (transport) {
    case ESP_IRIS_TRANSPORT_KIND_USB:
        return "usb";
    case ESP_IRIS_TRANSPORT_KIND_TCP:
        return "tcp";
    default:
        return "unknown";
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_ERROR_CHECK(esp_iris_mark_services_ready());

    char device_id[33];
    ESP_ERROR_CHECK(esp_iris_format_device_id(device_id));
    printf("ESP-Iris minimal example started: device_id=%s\n", device_id);

    while (true) {
        esp_iris_status_t status;
        ESP_ERROR_CHECK(esp_iris_get_status(&status));
        printf("status transport=%s lifecycle=%d link=%d session=%d "
               "uptime_us=%" PRIu64 " rx=%" PRIu32 " tx=%" PRIu32
               " dropped_log_bytes=%" PRIu32 " stack_free_min=%" PRIu32
               "\n",
               transport_name(status.transport), status.lifecycle,
               status.link_connected, status.session_ready, status.uptime_us,
               status.rx_frames, status.tx_frames, status.log_dropped_bytes,
               status.task_stack_free_min_bytes);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
