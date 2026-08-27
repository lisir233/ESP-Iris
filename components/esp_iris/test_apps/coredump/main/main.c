#include "esp_iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TEST_NVS_NAMESPACE "iris_test"
#define TEST_WIFI_READY BIT0

static EventGroupHandle_t s_wifi_events;
static char s_ip_address[16];
static TaskHandle_t s_job_task;
static esp_iris_job_handle_t s_job;

static void put_le32(uint8_t out[4], uint32_t value)
{
    out[0] = value & 0xffU;
    out[1] = (value >> 8) & 0xffU;
    out[2] = (value >> 16) & 0xffU;
    out[3] = (value >> 24) & 0xffU;
}

static esp_err_t test_nvs_set_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TEST_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return err;
}

static uint8_t test_nvs_get_u8(const char *key)
{
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open(TEST_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, key, &value);
        nvs_close(handle);
    }
    return value;
}

static esp_err_t test_nvs_write_recovery(uint32_t running,
                                         uint32_t target)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(TEST_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "last_good", running);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "target", target);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    return err;
}

static uint32_t test_nvs_get_u32(const char *key)
{
    nvs_handle_t handle;
    uint32_t value = 0;
    if (nvs_open(TEST_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u32(handle, key, &value);
        nvs_close(handle);
    }
    return value;
}

static bool test_app_partition_address_valid(uint32_t address)
{
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; iterator != NULL; iterator = esp_partition_next(iterator)) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (partition != NULL && partition->address == address) {
            esp_partition_iterator_release(iterator);
            return true;
        }
    }
    esp_partition_iterator_release(iterator);
    return false;
}

static esp_err_t test_recovery_sanitize(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t stored_last_good = test_nvs_get_u32("last_good");
    const uint32_t stored_target = test_nvs_get_u32("target");
    const uint32_t last_good =
        test_app_partition_address_valid(stored_last_good)
            ? stored_last_good : running->address;
    const uint32_t target = stored_target == 0 ||
                            test_app_partition_address_valid(stored_target)
        ? stored_target : 0;
    if (last_good == stored_last_good && target == stored_target) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(test_nvs_write_recovery(last_good, target),
                        "iris_test", "recovery metadata");
    return test_nvs_set_u8("planned", 0);
}

static esp_err_t ensure_initial_pairing_token(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("esp_iris", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t token[32];
        size_t size = sizeof(token);
        err = nvs_get_blob(handle, "pair_token", token, &size);
        memset(token, 0, sizeof(token));
        nvs_close(handle);
        if (err == ESP_OK && size == 32) {
            return ESP_OK;
        }
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_INVALID_SIZE) {
        return err;
    }
    return esp_iris_pairing_token_set(CONFIG_ESP_IRIS_TEST_PAIRING_TOKEN);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id,
                       void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        (void)esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR,
                 IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, TEST_WIFI_READY);
    }
}

static esp_err_t wifi_start(void)
{
    if (CONFIG_ESP_IRIS_TEST_WIFI_SSID[0] == '\0' ||
        CONFIG_ESP_IRIS_TEST_WIFI_PASSWORD[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), "iris_test", "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), "iris_test",
                        "event loop");
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), "iris_test", "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL),
        "iris_test", "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL),
        "iris_test", "ip handler");
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, CONFIG_ESP_IRIS_TEST_WIFI_SSID,
            sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_ESP_IRIS_TEST_WIFI_PASSWORD,
            sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), "iris_test",
                        "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), "iris_test",
                        "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config),
                        "iris_test", "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "iris_test", "wifi start");
    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, TEST_WIFI_READY, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(30000));
    return (bits & TEST_WIFI_READY) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
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

static void job_cancel(void *user_ctx)
{
    (void)user_ctx;
    if (s_job_task != NULL) {
        xTaskNotifyGive(s_job_task);
    }
}

static void job_runner(void *arg)
{
    esp_iris_job_handle_t job = arg;
    for (uint16_t progress = 50; progress <= 1000; progress += 50) {
        if (esp_iris_job_cancel_requested(job)) {
            (void)esp_iris_job_finish(job, ESP_ERR_INVALID_STATE);
            s_job_task = NULL;
            vTaskDelete(NULL);
        }
        (void)esp_iris_job_update(job, progress);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    }
    (void)esp_iris_job_finish(job, ESP_OK);
    s_job_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_job_rpc(const esp_iris_rpc_request_t *request,
                               uint8_t *response, size_t response_capacity,
                               size_t *response_size, void *user_ctx)
{
    (void)request;
    (void)user_ctx;
    if (response_capacity < 4 || s_job_task != NULL) {
        return s_job_task != NULL ? ESP_ERR_INVALID_STATE
                                  : ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_iris_job_create(1, job_cancel, NULL, &s_job);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(job_runner, "iris_test_job", 2048, s_job, 4,
                    &s_job_task) != pdPASS) {
        (void)esp_iris_job_finish(s_job, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    esp_iris_job_info_t info;
    ESP_RETURN_ON_ERROR(esp_iris_job_get_info(s_job, &info), "iris_test",
                        "job info");
    put_le32(response, info.id);
    *response_size = 4;
    return ESP_OK;
}

static esp_err_t rotate_token_rpc(const esp_iris_rpc_request_t *request,
                                  uint8_t *response,
                                  size_t response_capacity,
                                  size_t *response_size, void *user_ctx)
{
    (void)request;
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    *response_size = 0;
    return esp_iris_pairing_token_set(
        CONFIG_ESP_IRIS_TEST_NEXT_PAIRING_TOKEN);
}

static esp_err_t healthy_rpc(const esp_iris_rpc_request_t *request,
                             uint8_t *response, size_t response_capacity,
                             size_t *response_size, void *user_ctx)
{
    (void)request;
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    *response_size = 0;
    return esp_iris_mark_healthy();
}

static esp_err_t arm_crash_rpc(const esp_iris_rpc_request_t *request,
                               uint8_t *response, size_t response_capacity,
                               size_t *response_size, void *user_ctx)
{
    (void)request;
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    *response_size = 0;
    return test_nvs_set_u8("crash_ota", 1);
}

static esp_err_t state_rpc(const esp_iris_rpc_request_t *request,
                           uint8_t *response, size_t response_capacity,
                           size_t *response_size, void *user_ctx)
{
    (void)request;
    (void)user_ctx;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL || response_capacity < 28) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(response, 0, 28);
    put_le32(response, running->address);
    put_le32(response + 4, test_nvs_get_u32("last_good"));
    put_le32(response + 8, test_nvs_get_u32("target"));
    response[12] = running->subtype;
    response[13] = test_nvs_get_u8("planned");
    response[14] = test_nvs_get_u8("crash_ota");
    const size_t label_size = strnlen(running->label, 12);
    response[15] = (uint8_t)label_size;
    memcpy(response + 16, running->label, label_size);
    *response_size = 28;
    return ESP_OK;
}

static esp_err_t screen_begin(const esp_iris_media_desc_t *requested,
                              esp_iris_media_desc_t *actual,
                              uint32_t *total_size, void *user_ctx)
{
    (void)requested;
    (void)user_ctx;
    *actual = (esp_iris_media_desc_t) {
        .width = 2,
        .height = 2,
        .stride = 4,
        .format = ESP_IRIS_PIXEL_FORMAT_RGB565,
    };
    *total_size = 8;
    return ESP_OK;
}

static esp_err_t screen_read(uint32_t offset, uint8_t *out, size_t capacity,
                             size_t *out_size, void *user_ctx)
{
    (void)user_ctx;
    static const uint8_t pixels[8] = {
        0x00, 0xf8, 0xe0, 0x07, 0x1f, 0x00, 0xff, 0xff,
    };
    if (offset >= sizeof(pixels) || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t size = sizeof(pixels) - offset;
    if (size > capacity) {
        size = capacity;
    }
    memcpy(out, pixels + offset, size);
    *out_size = size;
    return ESP_OK;
}

esp_err_t esp_iris_platform_mark_healthy(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running != NULL) {
            err = test_nvs_write_recovery(running->address, 0);
        }
    }
    if (err == ESP_OK) {
        err = test_nvs_set_u8("planned", 0);
    }
    return err;
}

esp_err_t esp_iris_platform_mark_planned_restart(void)
{
    return test_nvs_set_u8("planned", 1);
}

esp_err_t esp_iris_platform_prepare_ota(uint32_t running_address,
                                        uint32_t target_address)
{
    if (running_address == 0 || target_address == 0 ||
        running_address == target_address) {
        return ESP_ERR_INVALID_ARG;
    }
    return test_nvs_write_recovery(running_address, target_address);
}

static void maybe_crash_pending_ota(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL &&
        running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        running->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX &&
        test_nvs_get_u8("crash_ota") != 0) {
        (void)test_nvs_set_u8("crash_ota", 0);
        printf("IRIS_TEST_CRASHING_PENDING_OTA\n");
        fflush(stdout);
        abort();
    }
}

static void maybe_mark_pending_ota_healthy(void)
{
#ifndef CONFIG_ESP_IRIS_TEST_AUTO_MARK_HEALTHY
    return;
#else
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_iris_mark_healthy());
        printf("IRIS_TEST_AUTO_HEALTHY\n");
        fflush(stdout);
    }
#endif
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(test_recovery_sanitize());
    maybe_crash_pending_ota();
    ESP_ERROR_CHECK(ensure_initial_pairing_token());
    ESP_ERROR_CHECK(wifi_start());
    printf("IRIS_TEST_IP=%s\n", s_ip_address);
    fflush(stdout);

    const esp_iris_screen_backend_t screen = {
        .begin = screen_begin,
        .read = screen_read,
    };
    ESP_ERROR_CHECK(esp_iris_rpc_register(1, 1, echo_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(1, 2, start_job_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(1, 3, rotate_token_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(1, 4, healthy_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(1, 5, arm_crash_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(1, 6, state_rpc, NULL));
    ESP_ERROR_CHECK(esp_iris_screen_register(&screen));
    ESP_ERROR_CHECK(esp_iris_start());
    maybe_mark_pending_ota_healthy();
}
