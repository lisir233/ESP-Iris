#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../../../src/esp_iris_crash_recovery.c"

static uint8_t s_blob[sizeof(iris_crash_state_t)];
static size_t s_blob_size;
static bool s_blob_present;
static esp_reset_reason_t s_reason = ESP_RST_POWERON;
static int64_t s_now_us;
static unsigned s_boot_selections;
static unsigned s_restarts;

static esp_partition_t s_application = {
    .address = 0x120000,
    .size = 0x100000,
};
static esp_partition_t s_recovery = {
    .address = 0x20000,
    .size = 0x100000,
};
static const esp_partition_t *s_running = &s_application;
static const esp_partition_t *s_boot = &s_application;
static esp_app_desc_t s_description;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return storage;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    return semaphore != NULL ? pdTRUE : 0;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return semaphore != NULL ? pdTRUE : 0;
}

esp_err_t nvs_flash_init_partition(const char *partition)
{
    return partition != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t nvs_open_from_partition(const char *partition, const char *name,
                                  int mode, nvs_handle_t *out_handle)
{
    if (partition == NULL || name == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length)
{
    if (handle == 0 || key == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_blob_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out_value == NULL) {
        *length = s_blob_size;
        return ESP_OK;
    }
    assert(*length >= s_blob_size);
    memcpy(out_value, s_blob, s_blob_size);
    *length = s_blob_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    assert(handle != 0 && key != NULL && value != NULL);
    assert(length <= sizeof(s_blob));
    memcpy(s_blob, value, length);
    s_blob_size = length;
    s_blob_present = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    return handle != 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle != 0);
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return s_running;
}

const esp_partition_t *esp_ota_get_boot_partition(void)
{
    return s_boot;
}

esp_partition_iterator_t esp_partition_find(unsigned type, unsigned subtype,
                                            const char *label)
{
    return &s_recovery;
}

const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator)
{
    return iterator;
}

esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator)
{
    return NULL;
}

void esp_partition_iterator_release(esp_partition_iterator_t iterator)
{
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    s_boot = partition;
    ++s_boot_selections;
    return ESP_OK;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_description;
}

esp_reset_reason_t esp_reset_reason(void)
{
    return s_reason;
}

void esp_restart(void)
{
    ++s_restarts;
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

esp_err_t esp_iris_platform_select_recovery_target(uint32_t *target_address)
{
    *target_address = s_recovery.address;
    return ESP_OK;
}

static iris_runtime_t probe(esp_reset_reason_t reason)
{
    iris_runtime_t runtime = {0};
    s_reason = reason;
    assert(iris_crash_recovery_probe(&runtime) == ESP_OK);
    return runtime;
}

int main(void)
{
    memset(s_description.app_elf_sha256, 0xa5,
           sizeof(s_description.app_elf_sha256));

    iris_runtime_t runtime = probe(ESP_RST_POWERON);
    assert(runtime.crash_count == 0);

    runtime = probe(ESP_RST_PANIC);
    assert(runtime.crash_count == 1);
    assert(runtime.crash_origin_reset_reason == ESP_RST_PANIC);
    assert(runtime.crash_failed_app_address == s_application.address);
    assert(!runtime.crash_loop_triggered);

    runtime = probe(ESP_RST_SW);
    assert(runtime.crash_count == 1);
    assert(!runtime.previous_boot_planned);

    assert(iris_crash_recovery_mark_planned(&runtime) == ESP_OK);
    runtime = probe(ESP_RST_SW);
    assert(runtime.previous_boot_planned);
    assert(runtime.crash_count == 1);

    runtime = probe(ESP_RST_TASK_WDT);
    assert(runtime.crash_count == 2);
    assert(s_restarts == 0);

    runtime = probe(ESP_RST_BROWNOUT);
    assert(runtime.crash_count == 2);

    runtime = probe(ESP_RST_CPU_LOCKUP);
    assert(runtime.crash_count == 3);
    assert(runtime.crash_loop_triggered);
    assert(runtime.crash_recovery_pending);
    assert(s_boot_selections == 1);
    assert(s_boot == &s_recovery);
    assert(s_restarts == 1);

    s_running = &s_recovery;
    memset(s_description.app_elf_sha256, 0x5a,
           sizeof(s_description.app_elf_sha256));
    runtime = probe(ESP_RST_SW);
    assert(runtime.previous_boot_planned);
    assert(runtime.crash_count == 3);
    assert(runtime.crash_failed_app_address == s_application.address);
    assert(runtime.crash_recovery_pending);
    assert(s_restarts == 1);

    assert(iris_crash_recovery_reset(&runtime) == ESP_OK);
    assert(runtime.crash_count == 0);
    assert(!runtime.crash_loop_triggered);
    assert(!runtime.crash_recovery_pending);

    s_running = &s_application;
    s_boot = &s_application;
    memset(s_description.app_elf_sha256, 0xa5,
           sizeof(s_description.app_elf_sha256));
    runtime = probe(ESP_RST_POWERON);
    runtime = probe(ESP_RST_PANIC);
    assert(runtime.crash_count == 1);
    s_now_us = runtime.crash_stable_deadline_us;
    iris_crash_recovery_poll(&runtime, s_now_us);
    assert(runtime.crash_count == 0);
    return 0;
}
