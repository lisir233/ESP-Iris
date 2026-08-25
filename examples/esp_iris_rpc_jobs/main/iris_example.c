#include "iris_example.h"

#include <stdio.h>
#include <string.h>

#include "esp_iris.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LONG_JOB_SERVICE_ID 0x1100U
#define LONG_JOB_METHOD_ID  1U
#define LONG_JOB_KIND       0x1100U

static portMUX_TYPE s_job_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_job_task;
static esp_iris_job_handle_t s_active_job;

static void put_le32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static esp_err_t echo_rpc(const esp_iris_rpc_request_t *request,
                          uint8_t *response, size_t response_capacity,
                          size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size > response_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response, request->payload, request->payload_size);
    *response_size = request->payload_size;
    return ESP_OK;
}

static esp_err_t info_rpc(const esp_iris_rpc_request_t *request,
                          uint8_t *response, size_t response_capacity,
                          size_t *response_size, void *user_ctx)
{
    (void)request;
    (void)user_ctx;
    esp_iris_status_t status;
    char device_id[33];
    esp_err_t err = esp_iris_get_status(&status);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_iris_format_device_id(device_id);
    if (err != ESP_OK) {
        return err;
    }
    const int written = snprintf(
        (char *)response, response_capacity,
        "{\"device_id\":\"%s\",\"idf_version\":\"%s\","
        "\"transport\":\"usb\",\"uptime_us\":%llu,"
        "\"session_ready\":%s,\"free_heap\":%lu}",
        device_id, esp_get_idf_version(),
        (unsigned long long)status.uptime_us,
        status.session_ready ? "true" : "false",
        (unsigned long)esp_get_free_heap_size());
    if (written < 0 || (size_t)written >= response_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    *response_size = (size_t)written;
    return ESP_OK;
}

static void long_job_cancel(void *user_ctx)
{
    (void)user_ctx;
    TaskHandle_t task;
    taskENTER_CRITICAL(&s_job_lock);
    task = s_job_task;
    taskEXIT_CRITICAL(&s_job_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

static void long_job_task(void *arg)
{
    esp_iris_job_handle_t job = arg;
    for (uint16_t progress = 0; progress <= 1000; progress += 25) {
        if (esp_iris_job_cancel_requested(job)) {
            (void)esp_iris_job_finish(job, ESP_ERR_INVALID_STATE);
            break;
        }
        (void)esp_iris_job_update(job, progress);
        if (progress == 1000) {
            (void)esp_iris_job_finish(job, ESP_OK);
            break;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    }

    taskENTER_CRITICAL(&s_job_lock);
    s_job_task = NULL;
    s_active_job = NULL;
    taskEXIT_CRITICAL(&s_job_lock);
    vTaskDelete(NULL);
}

static esp_err_t start_long_job_rpc(const esp_iris_rpc_request_t *request,
                                    uint8_t *response,
                                    size_t response_capacity,
                                    size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (response_capacity < sizeof(uint32_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    taskENTER_CRITICAL(&s_job_lock);
    const bool busy = s_job_task != NULL || s_active_job != NULL;
    taskEXIT_CRITICAL(&s_job_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_iris_job_handle_t job;
    esp_err_t err = esp_iris_job_create(LONG_JOB_KIND, long_job_cancel,
                                        NULL, &job);
    if (err != ESP_OK) {
        return err;
    }

    TaskHandle_t task = NULL;
    if (xTaskCreate(long_job_task, "iris_long_job", 2048, job, 4,
                    &task) != pdPASS) {
        (void)esp_iris_job_finish(job, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_job_lock);
    s_active_job = job;
    s_job_task = task;
    taskEXIT_CRITICAL(&s_job_lock);

    esp_iris_job_info_t info;
    err = esp_iris_job_get_info(job, &info);
    if (err != ESP_OK) {
        return err;
    }
    put_le32(response, info.id);
    *response_size = sizeof(uint32_t);
    return ESP_OK;
}

void iris_example_start(void)
{
    ESP_ERROR_CHECK(esp_iris_rpc_register(1, 1, echo_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(1, 2, info_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(LONG_JOB_SERVICE_ID,
                                          LONG_JOB_METHOD_ID,
                                          start_long_job_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_start());
    ESP_ERROR_CHECK(esp_iris_mark_services_ready());
    printf("ESP-Iris RPC/Job example ready: system.echo=1/1, "
           "system.info=1/2, long-job=0x1100/1\n");
}
