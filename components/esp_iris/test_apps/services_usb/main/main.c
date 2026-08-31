#include "esp_iris.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_assets.h"
#include "wear_levelling.h"

#define TEST_SERVICE_ID 0x7FFEU
#define TEST_STATE_METHOD 1U
#define TEST_LOG_BURST_METHOD 2U
#define TEST_LIFECYCLE_METHOD 3U
#define TEST_MEDIA_METHOD 4U
#define TEST_STOP_FOR_FLASH_METHOD 5U
#define TEST_BOUNDARY_METHOD 6U
#define POINTER_SERVICE_ID 0x1001U
#define POINTER_METHOD_ID 1U
#define FILE_MOUNT_PATH "/files"
#define READ_ONLY_PATH FILE_MOUNT_PATH "/readonly"
#define ATOMIC_MOUNT_PATH "/atomic"
#define TEST_STATE_SCHEMA 1U
#define TEST_STATE_WORDS 21U

typedef struct {
    uint32_t start_count;
    uint32_t stop_count;
    uint32_t register_count;
    uint32_t unregister_count;
    uint32_t stdout_records;
    uint32_t stderr_records;
    uint32_t log_bytes;
    uint32_t image_frames;
    uint32_t audio_frames;
    uint32_t media_errors;
    uint32_t jobs_created;
    uint32_t jobs_finished;
    uint32_t jobs_cancelled;
    esp_err_t last_error;
    uint32_t pointer_count;
    uint8_t last_pointer[12];
    uint16_t image_format;
    uint16_t audio_format;
    uint8_t media_period_ms;
    esp_err_t duplicate_rpc_error;
    esp_err_t rpc_table_full_error;
    esp_err_t volume_table_full_error;
} test_state_t;

typedef struct {
    esp_iris_job_handle_t handle;
    TaskHandle_t task;
    uint8_t behavior;
    uint16_t duration_ms;
    bool active;
} test_job_t;

static const char *TAG = "iris_services_e2e";
static test_state_t s_state = {
    .image_format = ESP_IRIS_PIXEL_FORMAT_RGB565,
    .audio_format = ESP_IRIS_AUDIO_FORMAT_PCM_S16LE,
    .media_period_ms = 20,
};
static test_job_t s_jobs[CONFIG_ESP_IRIS_MAX_JOBS];
static TaskHandle_t s_lifecycle_task;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static const esp_iris_screen_backend_t s_screen;

static void put_le16(uint8_t out[2], uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint16_t get_le16(const uint8_t in[2])
{
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}

static uint32_t get_le32(const uint8_t in[4])
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static void seed_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        file = fopen(path, "wb");
        if (file != NULL) {
            (void)fwrite(contents, 1, strlen(contents), file);
        }
    }
    if (file != NULL) {
        (void)fclose(file);
    } else {
        ESP_LOGW(TAG, "unable to seed %s", path);
    }
}

static void register_file_volumes(void)
{
    const esp_vfs_fat_mount_config_t fat = {
        .format_if_mount_failed = true,
        .max_files = 12,
        .allocation_unit_size = 4096,
    };
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(
        FILE_MOUNT_PATH, "storage", &fat, &s_wl_handle));
    if (mkdir(READ_ONLY_PATH, 0755) != 0) {
        struct stat metadata;
        ESP_ERROR_CHECK(stat(READ_ONLY_PATH, &metadata) == 0 &&
                                S_ISDIR(metadata.st_mode)
                            ? ESP_OK
                            : ESP_FAIL);
    }
    seed_file(FILE_MOUNT_PATH "/README.txt",
              "ESP-Iris hardware file-service fixture\n");
    seed_file(READ_ONLY_PATH "/NOTICE.txt", "read-only Iris volume\n");

    const esp_vfs_littlefs_conf_t littlefs = {
        .base_path = ATOMIC_MOUNT_PATH,
        .partition_label = "atomic",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&littlefs));
    seed_file(ATOMIC_MOUNT_PATH "/current.txt", "atomic baseline\n");

    const esp_iris_file_volume_config_t volumes[] = {
        {
            .id = "fs",
            .base_path = FILE_MOUNT_PATH,
            .capabilities = ESP_IRIS_FILE_VOLUME_READ |
                            ESP_IRIS_FILE_VOLUME_LIST |
                            ESP_IRIS_FILE_VOLUME_MTIME |
                            ESP_IRIS_FILE_VOLUME_WRITE |
                            ESP_IRIS_FILE_VOLUME_DELETE |
                            ESP_IRIS_FILE_VOLUME_MKDIR |
                            ESP_IRIS_FILE_VOLUME_RENAME,
        },
        {
            .id = "ro",
            .base_path = READ_ONLY_PATH,
            .capabilities = ESP_IRIS_FILE_VOLUME_READ |
                            ESP_IRIS_FILE_VOLUME_LIST |
                            ESP_IRIS_FILE_VOLUME_MTIME,
        },
        {
            .id = "atomic",
            .base_path = ATOMIC_MOUNT_PATH,
            .capabilities = ESP_IRIS_FILE_VOLUME_READ |
                            ESP_IRIS_FILE_VOLUME_LIST |
                            ESP_IRIS_FILE_VOLUME_MTIME |
                            ESP_IRIS_FILE_VOLUME_WRITE |
                            ESP_IRIS_FILE_VOLUME_DELETE |
                            ESP_IRIS_FILE_VOLUME_MKDIR |
                            ESP_IRIS_FILE_VOLUME_RENAME |
                            ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE,
        },
    };
    for (size_t i = 0; i < sizeof(volumes) / sizeof(volumes[0]); ++i) {
        ESP_ERROR_CHECK(esp_iris_file_volume_register(&volumes[i]));
    }

    const esp_iris_file_volume_config_t scratch = {
        .id = "scratch",
        .base_path = FILE_MOUNT_PATH,
        .capabilities = ESP_IRIS_FILE_VOLUME_READ,
    };
    const esp_iris_file_volume_config_t overflow = {
        .id = "overflow",
        .base_path = FILE_MOUNT_PATH,
        .capabilities = ESP_IRIS_FILE_VOLUME_READ,
    };
    ESP_ERROR_CHECK(esp_iris_file_volume_register(&scratch));
    s_state.volume_table_full_error =
        esp_iris_file_volume_register(&overflow);
    ESP_ERROR_CHECK(esp_iris_file_volume_unregister(scratch.id));
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

static esp_err_t delayed_echo_rpc(const esp_iris_rpc_request_t *request,
                                  uint8_t *response,
                                  size_t response_capacity,
                                  size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size < 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint16_t delay_ms = get_le16(request->payload);
    if (delay_ms > 5000 || request->payload_size - 2U > response_capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    memcpy(response, request->payload + 2, request->payload_size - 2U);
    *response_size = request->payload_size - 2U;
    return ESP_OK;
}

static esp_err_t error_rpc(const esp_iris_rpc_request_t *request,
                           uint8_t *response, size_t response_capacity,
                           size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    *response_size = 0;
    if (request->payload_size != 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    return (esp_err_t)get_le32(request->payload);
}

static void job_cancel(void *user_ctx)
{
    test_job_t *job = user_ctx;
    if (job != NULL && job->task != NULL) {
        xTaskNotifyGive(job->task);
    }
}

static void job_task(void *arg)
{
    test_job_t *job = arg;
    const TickType_t step = pdMS_TO_TICKS(
        job->duration_ms < 10 ? 10 : job->duration_ms / 10U);
    for (uint16_t progress = 100; progress <= 1000; progress += 100) {
        (void)ulTaskNotifyTake(pdTRUE, step);
        if (esp_iris_job_cancel_requested(job->handle)) {
            ++s_state.jobs_cancelled;
            (void)esp_iris_job_finish(job->handle, ESP_ERR_INVALID_STATE);
            ++s_state.jobs_finished;
            job->active = false;
            job->task = NULL;
            vTaskDelete(NULL);
        }
        (void)esp_iris_job_update(job->handle, progress);
    }
    (void)esp_iris_job_finish(
        job->handle, job->behavior == 0 ? ESP_OK : ESP_FAIL);
    ++s_state.jobs_finished;
    job->active = false;
    job->task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_job_rpc(const esp_iris_rpc_request_t *request,
                               uint8_t *response, size_t response_capacity,
                               size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size != 0 && request->payload_size != 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (response_capacity < 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    test_job_t *slot = NULL;
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_JOBS; ++i) {
        if (!s_jobs[i].active) {
            slot = &s_jobs[i];
            break;
        }
    }
    if (slot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->behavior = request->payload_size == 4 ? request->payload[0] : 0;
    slot->duration_ms = request->payload_size == 4
        ? get_le16(request->payload + 2) : 500;
    if (slot->behavior > 1 || slot->duration_ms > 30000) {
        slot->active = false;
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_iris_job_create(0x7FFEU, job_cancel, slot,
                                        &slot->handle);
    if (err != ESP_OK) {
        slot->active = false;
        return err;
    }
    if (xTaskCreate(job_task, "iris_e2e_job", 2048, slot, 4,
                    &slot->task) != pdPASS) {
        (void)esp_iris_job_finish(slot->handle, ESP_ERR_NO_MEM);
        slot->active = false;
        return ESP_ERR_NO_MEM;
    }
    esp_iris_job_info_t info;
    ESP_RETURN_ON_ERROR(esp_iris_job_get_info(slot->handle, &info), TAG,
                        "job info");
    ++s_state.jobs_created;
    put_le32(response, info.id);
    *response_size = 4;
    return ESP_OK;
}

static esp_err_t pointer_rpc(const esp_iris_rpc_request_t *request,
                             uint8_t *response, size_t response_capacity,
                             size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size != sizeof(s_state.last_pointer) ||
        response_capacity < sizeof(s_state.last_pointer)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_state.last_pointer, request->payload,
           sizeof(s_state.last_pointer));
    int16_t x = (int16_t)get_le16(s_state.last_pointer + 2);
    int16_t y = (int16_t)get_le16(s_state.last_pointer + 4);
    x = x < 0 ? 0 : (x > 479 ? 479 : x);
    y = y < 0 ? 0 : (y > 479 ? 479 : y);
    put_le16(s_state.last_pointer + 2, (uint16_t)x);
    put_le16(s_state.last_pointer + 4, (uint16_t)y);
    ++s_state.pointer_count;
    memcpy(response, s_state.last_pointer, sizeof(s_state.last_pointer));
    *response_size = sizeof(s_state.last_pointer);
    return ESP_OK;
}

static esp_err_t state_rpc(const esp_iris_rpc_request_t *request,
                           uint8_t *response, size_t response_capacity,
                           size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    const size_t required = 4U + TEST_STATE_WORDS * 4U +
                            sizeof(s_state.last_pointer);
    if (request->payload_size != 0 || response_capacity < required) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_iris_status_t status = {0};
    const esp_err_t status_err = esp_iris_get_status(&status);
    if (status_err != ESP_OK) {
        return status_err;
    }
    put_le16(response, TEST_STATE_SCHEMA);
    put_le16(response + 2, (uint16_t)status.transport);
    const uint32_t words[TEST_STATE_WORDS] = {
        status.started ? 1U : 0U,
        (uint32_t)status.lifecycle,
        s_state.start_count,
        s_state.stop_count,
        s_state.register_count,
        s_state.unregister_count,
        s_state.stdout_records,
        s_state.stderr_records,
        s_state.log_bytes,
        s_state.image_frames,
        s_state.audio_frames,
        s_state.media_errors,
        s_state.jobs_created,
        s_state.jobs_finished,
        s_state.jobs_cancelled,
        (uint32_t)s_state.last_error,
        s_state.pointer_count,
        status.invalid_frames,
        status.log_dropped_bytes,
        status.task_stack_free_min_bytes,
        status.internal_heap_used_bytes,
    };
    for (size_t i = 0; i < TEST_STATE_WORDS; ++i) {
        put_le32(response + 4U + i * 4U, words[i]);
    }
    memcpy(response + 4U + TEST_STATE_WORDS * 4U, s_state.last_pointer,
           sizeof(s_state.last_pointer));
    *response_size = required;
    return ESP_OK;
}

static esp_err_t log_burst_rpc(const esp_iris_rpc_request_t *request,
                               uint8_t *response, size_t response_capacity,
                               size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    if (request->payload_size != 8) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint16_t stdout_count = get_le16(request->payload);
    const uint16_t stderr_count = get_le16(request->payload + 2);
    const uint16_t record_size = get_le16(request->payload + 4);
    if ((uint32_t)stdout_count + stderr_count > 4096U ||
        record_size == 0 || record_size > 256U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t record[256];
    for (size_t i = 0; i < record_size; ++i) {
        record[i] = (uint8_t)('A' + i % 26U);
    }
    for (uint16_t i = 0; i < stdout_count; ++i) {
        (void)fwrite(record, 1, record_size, stdout);
    }
    for (uint16_t i = 0; i < stderr_count; ++i) {
        (void)fwrite(record, 1, record_size, stderr);
    }
    (void)fflush(stdout);
    (void)fflush(stderr);
    s_state.stdout_records += stdout_count;
    s_state.stderr_records += stderr_count;
    s_state.log_bytes += ((uint32_t)stdout_count + stderr_count) * record_size;
    *response_size = 0;
    return ESP_OK;
}

static esp_err_t media_configure_rpc(
    const esp_iris_rpc_request_t *request, uint8_t *response,
    size_t response_capacity, size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    if (request->payload_size != 8) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t channel = request->payload[0];
    const uint16_t format = get_le16(request->payload + 1);
    const uint8_t period_ms = request->payload[3];
    const bool valid_image = channel == ESP_IRIS_CHANNEL_IMAGE &&
        (format == ESP_IRIS_PIXEL_FORMAT_RGB565 ||
         format == ESP_IRIS_PIXEL_FORMAT_RGB888 ||
         format == ESP_IRIS_PIXEL_FORMAT_JPEG ||
         format == ESP_IRIS_PIXEL_FORMAT_PNG);
    const bool valid_audio = channel == ESP_IRIS_CHANNEL_AUDIO &&
        (format == ESP_IRIS_AUDIO_FORMAT_PCM_S16LE ||
         format == ESP_IRIS_AUDIO_FORMAT_OPUS);
    if ((!valid_image && !valid_audio) || period_ms < 10 || period_ms > 250) {
        return ESP_ERR_INVALID_ARG;
    }
    if (valid_image) {
        s_state.image_format = format;
    } else {
        s_state.audio_format = format;
    }
    s_state.media_period_ms = period_ms;
    *response_size = 0;
    return ESP_OK;
}

static esp_err_t dummy_rpc(const esp_iris_rpc_request_t *request,
                           uint8_t *response, size_t response_capacity,
                           size_t *response_size, void *user_ctx)
{
    (void)request;
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    *response_size = 0;
    return ESP_OK;
}

static esp_err_t boundary_rpc(const esp_iris_rpc_request_t *request,
                              uint8_t *response, size_t response_capacity,
                              size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size != 0 || response_capacity < 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_iris_job_handle_t jobs[CONFIG_ESP_IRIS_MAX_JOBS];
    esp_err_t job_full = ESP_FAIL;
    size_t created = 0;
    for (; created < CONFIG_ESP_IRIS_MAX_JOBS; ++created) {
        const esp_err_t err = esp_iris_job_create(0x7FFDU, NULL, NULL,
                                                  &jobs[created]);
        if (err != ESP_OK) {
            job_full = err;
            break;
        }
    }
    if (created == CONFIG_ESP_IRIS_MAX_JOBS) {
        esp_iris_job_handle_t overflow;
        job_full = esp_iris_job_create(0x7FFDU, NULL, NULL, &overflow);
    }
    for (size_t i = 0; i < created; ++i) {
        (void)esp_iris_job_finish(jobs[i], ESP_OK);
    }
    static const uint8_t sample[CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES + 1U] = {0};
    const esp_iris_media_desc_t description = {
        .width = 1,
        .height = 1,
        .stride = 2,
        .format = ESP_IRIS_PIXEL_FORMAT_RGB565,
    };
    const int32_t errors[8] = {
        esp_iris_rpc_register(0, 1, dummy_rpc, NULL),
        s_state.duplicate_rpc_error,
        s_state.rpc_table_full_error,
        esp_iris_job_create(0, NULL, NULL, &jobs[0]),
        job_full,
        s_state.volume_table_full_error,
        esp_iris_media_submit(ESP_IRIS_CHANNEL_CONTROL, &description, 1, 0,
                              sample, 1),
        esp_iris_media_submit(ESP_IRIS_CHANNEL_IMAGE, &description, 1, 0,
                              sample, sizeof(sample)),
    };
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        put_le32(response + i * 4U, (uint32_t)errors[i]);
    }
    *response_size = 32;
    return ESP_OK;
}

static esp_err_t register_services(void);

static void lifecycle_task(void *arg)
{
    const bool restart = (bool)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_err_t err = esp_iris_stop();
    if (err == ESP_OK) {
        ++s_state.stop_count;
    }
    if (err == ESP_OK && restart) {
        const uint16_t methods[][2] = {
            {1, 1}, {1, 2}, {1, 3}, {1, 7},
            {POINTER_SERVICE_ID, POINTER_METHOD_ID},
            {TEST_SERVICE_ID, TEST_STATE_METHOD},
            {TEST_SERVICE_ID, TEST_LOG_BURST_METHOD},
            {TEST_SERVICE_ID, TEST_LIFECYCLE_METHOD},
            {TEST_SERVICE_ID, TEST_MEDIA_METHOD},
            {TEST_SERVICE_ID, TEST_STOP_FOR_FLASH_METHOD},
            {TEST_SERVICE_ID, TEST_BOUNDARY_METHOD},
        };
        for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
            err = esp_iris_rpc_unregister(methods[i][0], methods[i][1]);
            if (err != ESP_OK) {
                break;
            }
        }
        if (err == ESP_OK) {
            err = esp_iris_screen_unregister(NULL);
        }
        if (err == ESP_OK) {
            ++s_state.unregister_count;
            err = register_services();
        }
        if (err == ESP_OK) {
            err = esp_iris_start();
        }
        if (err == ESP_OK) {
            ++s_state.start_count;
        }
    }
    s_state.last_error = err;
    s_lifecycle_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t schedule_lifecycle(const esp_iris_rpc_request_t *request,
                                    size_t *response_size, bool restart)
{
    if (request->payload_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_lifecycle_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(lifecycle_task, "iris_e2e_cycle", 3072,
                    (void *)(uintptr_t)restart, 5,
                    &s_lifecycle_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    *response_size = 0;
    return ESP_OK;
}

static esp_err_t lifecycle_rpc(const esp_iris_rpc_request_t *request,
                               uint8_t *response, size_t response_capacity,
                               size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    return schedule_lifecycle(request, response_size, true);
}

static esp_err_t stop_for_flash_rpc(const esp_iris_rpc_request_t *request,
                                    uint8_t *response,
                                    size_t response_capacity,
                                    size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    return schedule_lifecycle(request, response_size, false);
}

#define REGISTER_RPC(service, method, handler) do {                       \
    const esp_err_t register_error =                                      \
        esp_iris_rpc_register((service), (method), (handler), NULL);       \
    if (register_error != ESP_OK) {                                       \
        return register_error;                                            \
    }                                                                     \
} while (0)

static esp_err_t register_services(void)
{
    REGISTER_RPC(1, 1, echo_rpc);
    REGISTER_RPC(1, 2, start_job_rpc);
    REGISTER_RPC(1, 3, delayed_echo_rpc);
    REGISTER_RPC(1, 7, error_rpc);
    REGISTER_RPC(POINTER_SERVICE_ID, POINTER_METHOD_ID, pointer_rpc);
    REGISTER_RPC(TEST_SERVICE_ID, TEST_STATE_METHOD, state_rpc);
    REGISTER_RPC(TEST_SERVICE_ID, TEST_LOG_BURST_METHOD, log_burst_rpc);
    REGISTER_RPC(TEST_SERVICE_ID, TEST_LIFECYCLE_METHOD, lifecycle_rpc);
    REGISTER_RPC(TEST_SERVICE_ID, TEST_MEDIA_METHOD, media_configure_rpc);
    REGISTER_RPC(TEST_SERVICE_ID, TEST_STOP_FOR_FLASH_METHOD,
                 stop_for_flash_rpc);
    REGISTER_RPC(TEST_SERVICE_ID, TEST_BOUNDARY_METHOD, boundary_rpc);
    ESP_RETURN_ON_ERROR(esp_iris_screen_register(&s_screen), TAG,
                        "screen register");
    ++s_state.register_count;
    return ESP_OK;
}

static void exercise_rpc_table_boundary(void)
{
    s_state.duplicate_rpc_error = esp_iris_rpc_register(1, 1, dummy_rpc, NULL);
    uint16_t registered[CONFIG_ESP_IRIS_MAX_RPC_HANDLERS];
    size_t count = 0;
    for (; count < CONFIG_ESP_IRIS_MAX_RPC_HANDLERS; ++count) {
        const uint16_t method = (uint16_t)(count + 1U);
        const esp_err_t err = esp_iris_rpc_register(0x7FFDU, method,
                                                    dummy_rpc, NULL);
        if (err != ESP_OK) {
            s_state.rpc_table_full_error = err;
            break;
        }
        registered[count] = method;
    }
    if (count == CONFIG_ESP_IRIS_MAX_RPC_HANDLERS) {
        s_state.rpc_table_full_error = esp_iris_rpc_register(
            0x7FFDU, 0x7FFFU, dummy_rpc, NULL);
    }
    for (size_t i = 0; i < count; ++i) {
        ESP_ERROR_CHECK(esp_iris_rpc_unregister(0x7FFDU, registered[i]));
    }
}

static esp_err_t screen_begin(const esp_iris_media_desc_t *requested,
                              esp_iris_media_desc_t *actual,
                              uint32_t *total_size, void *user_ctx)
{
    (void)user_ctx;
    if (requested == NULL || actual == NULL || total_size == NULL ||
        requested->width > 2 || requested->height > 2) {
        return ESP_ERR_INVALID_ARG;
    }
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
    static const uint8_t pixels[8] = {
        0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xFF, 0xFF,
    };
    (void)user_ctx;
    if (out == NULL || out_size == NULL || offset >= sizeof(pixels) ||
        capacity == 0) {
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

static void screen_end(void *user_ctx)
{
    (void)user_ctx;
}

static const esp_iris_screen_backend_t s_screen = {
    .begin = screen_begin,
    .read = screen_read,
    .end = screen_end,
};

static void render_rgb565(uint8_t out[128], uint32_t frame_id)
{
    for (size_t i = 0; i < 64; ++i) {
        const uint16_t pixel = (uint16_t)(((i + frame_id) & 0x1FU) << 11) |
                               (uint16_t)((i & 0x3FU) << 5) |
                               (uint16_t)(frame_id & 0x1FU);
        put_le16(out + i * 2U, pixel);
    }
}

static void render_rgb888(uint8_t out[192], uint32_t frame_id)
{
    for (size_t i = 0; i < 64; ++i) {
        out[i * 3U] = (uint8_t)(i * 4U);
        out[i * 3U + 1U] = (uint8_t)(frame_id + i);
        out[i * 3U + 2U] = (uint8_t)(255U - i * 4U);
    }
}

static void media_task(void *arg)
{
    (void)arg;
    static uint8_t rgb565[128];
    static uint8_t rgb888[192];
    static const uint8_t pcm[] = {
        0x00, 0x00, 0x98, 0x08, 0xFB, 0x0F, 0x41, 0x15,
        0x98, 0x17, 0x41, 0x15, 0xFB, 0x0F, 0x98, 0x08,
        0x00, 0x00, 0x68, 0xF7, 0x05, 0xF0, 0xBF, 0xEA,
        0x68, 0xE8, 0xBF, 0xEA, 0x05, 0xF0, 0x68, 0xF7,
    };
    static const uint8_t opus[] = {0xF8, 0xFF, 0xFE};
    uint32_t image_id = 0;
    uint32_t audio_id = 0;
    while (true) {
        if (esp_iris_media_is_streaming(ESP_IRIS_CHANNEL_IMAGE)) {
            const uint8_t *data = NULL;
            size_t size = 0;
            esp_iris_media_desc_t description = {
                .width = 8,
                .height = 8,
                .format = s_state.image_format,
            };
            if (s_state.image_format == ESP_IRIS_PIXEL_FORMAT_RGB565) {
                description.stride = 16;
                render_rgb565(rgb565, image_id);
                data = rgb565;
                size = sizeof(rgb565);
            } else if (s_state.image_format == ESP_IRIS_PIXEL_FORMAT_RGB888) {
                description.stride = 24;
                render_rgb888(rgb888, image_id);
                data = rgb888;
                size = sizeof(rgb888);
            } else if (s_state.image_format == ESP_IRIS_PIXEL_FORMAT_JPEG) {
                description.quality = 75;
                data = iris_example_jpeg;
                size = iris_example_jpeg_size;
            } else {
                data = iris_example_png;
                size = iris_example_png_size;
            }
            const esp_err_t err = esp_iris_media_submit(
                ESP_IRIS_CHANNEL_IMAGE, &description, ++image_id, 0,
                data, size);
            if (err == ESP_OK) {
                ++s_state.image_frames;
            } else {
                ++s_state.media_errors;
                s_state.last_error = err;
            }
        }
        if (esp_iris_media_is_streaming(ESP_IRIS_CHANNEL_AUDIO)) {
            const esp_iris_media_desc_t description = {
                .width = 16000,
                .height = 1,
                .stride = 2,
                .format = s_state.audio_format,
            };
            const void *data = s_state.audio_format ==
                ESP_IRIS_AUDIO_FORMAT_OPUS ? (const void *)opus
                                           : (const void *)pcm;
            const size_t size = s_state.audio_format ==
                ESP_IRIS_AUDIO_FORMAT_OPUS ? sizeof(opus) : sizeof(pcm);
            const esp_err_t err = esp_iris_media_submit(
                ESP_IRIS_CHANNEL_AUDIO, &description, ++audio_id, 0,
                data, size);
            if (err == ESP_OK) {
                ++s_state.audio_frames;
            } else {
                ++s_state.media_errors;
                s_state.last_error = err;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(s_state.media_period_ms));
    }
}

void app_main(void)
{
    register_file_volumes();
    ESP_ERROR_CHECK(register_services());
    exercise_rpc_table_boundary();
    ESP_ERROR_CHECK(esp_iris_start());
    ++s_state.start_count;
    ESP_ERROR_CHECK(xTaskCreate(media_task, "iris_e2e_media", 3072, NULL, 4,
                                NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    esp_rom_printf("IRIS_SERVICES_READY schema=1 transport=%u volumes=3 "
                   "rpc=0x7ffe\n",
                   (unsigned)(
#if CONFIG_ESP_IRIS_TRANSPORT_USB
                       ESP_IRIS_TRANSPORT_KIND_USB
#else
                       ESP_IRIS_TRANSPORT_KIND_USB_SERIAL_JTAG
#endif
                   ));
}
