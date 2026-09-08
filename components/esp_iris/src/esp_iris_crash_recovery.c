#include "esp_iris_internal.h"

#include <limits.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#ifndef CONFIG_ESP_IRIS_CRASH_LOOP_LIMIT
#define CONFIG_ESP_IRIS_CRASH_LOOP_LIMIT 3
#endif
#ifndef CONFIG_ESP_IRIS_CRASH_LOOP_STABLE_MS
#define CONFIG_ESP_IRIS_CRASH_LOOP_STABLE_MS 30000
#endif

#define IRIS_CRASH_NVS_NAMESPACE "esp_iris"
#define IRIS_CRASH_NVS_KEY "crash_loop"
#define IRIS_CRASH_STATE_MAGIC 0x53495249UL /* "IRIS" on little endian. */
#define IRIS_CRASH_STATE_VERSION 1U

enum {
    IRIS_CRASH_FLAG_LAST_STARTED_VALID = 1U << 0,
    IRIS_CRASH_FLAG_FAILED_VALID = 1U << 1,
    IRIS_CRASH_FLAG_PLANNED_RESTART = 1U << 2,
    IRIS_CRASH_FLAG_RECOVERY_PENDING = 1U << 3,
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t flags;
    uint32_t count;
    uint32_t limit;
    uint32_t failure_reset_reason;
    uint32_t last_started_address;
    uint32_t planned_target_address;
    uint32_t failed_address;
    uint8_t last_started_sha256[32];
    uint8_t failed_sha256[32];
} iris_crash_state_t;

static StaticSemaphore_t s_crash_mutex_storage;
static SemaphoreHandle_t s_crash_mutex;

static SemaphoreHandle_t crash_mutex(void)
{
    /* The first call is made synchronously by esp_iris_boot_probe(). All
     * later callers run after that probe, so no dynamic allocation or lazy
     * initialization race is introduced here. */
    if (s_crash_mutex == NULL) {
        s_crash_mutex = xSemaphoreCreateMutexStatic(&s_crash_mutex_storage);
    }
    return s_crash_mutex;
}

static void state_init(iris_crash_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->magic = IRIS_CRASH_STATE_MAGIC;
    state->version = IRIS_CRASH_STATE_VERSION;
    state->size = sizeof(*state);
}

static bool state_valid(const iris_crash_state_t *state, size_t stored_size)
{
    return stored_size == sizeof(*state) &&
           state->magic == IRIS_CRASH_STATE_MAGIC &&
           state->version == IRIS_CRASH_STATE_VERSION &&
           state->size == sizeof(*state);
}

static esp_err_t state_open(nvs_handle_t *out_handle)
{
    esp_err_t err =
        nvs_flash_init_partition(CONFIG_ESP_IRIS_NVS_PARTITION_NAME);
    if (err != ESP_OK) {
        /* Iris must not erase a shared system-metadata partition implicitly. */
        return err;
    }
    return nvs_open_from_partition(CONFIG_ESP_IRIS_NVS_PARTITION_NAME,
                                   IRIS_CRASH_NVS_NAMESPACE, NVS_READWRITE,
                                   out_handle);
}

static esp_err_t state_read(nvs_handle_t handle, iris_crash_state_t *state,
                            bool *out_replaced)
{
    size_t size = 0;
    esp_err_t err = nvs_get_blob(handle, IRIS_CRASH_NVS_KEY, NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        state_init(state);
        *out_replaced = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (size != sizeof(*state)) {
        state_init(state);
        *out_replaced = true;
        return ESP_OK;
    }
    err = nvs_get_blob(handle, IRIS_CRASH_NVS_KEY, state, &size);
    if (err != ESP_OK) {
        return err;
    }
    if (!state_valid(state, size)) {
        state_init(state);
        *out_replaced = true;
    }
    return ESP_OK;
}

static esp_err_t state_write(nvs_handle_t handle,
                             const iris_crash_state_t *state)
{
    esp_err_t err = nvs_set_blob(handle, IRIS_CRASH_NVS_KEY, state,
                                 sizeof(*state));
    return err == ESP_OK ? nvs_commit(handle) : err;
}

static bool same_image(uint32_t first_address, const uint8_t first_sha[32],
                       uint32_t second_address, const uint8_t second_sha[32])
{
    return first_address == second_address &&
           memcmp(first_sha, second_sha, 32) == 0;
}

static uint32_t state_limit(const iris_crash_state_t *state)
{
    return state->limit >= 1 && state->limit <= 100
               ? state->limit
               : CONFIG_ESP_IRIS_CRASH_LOOP_LIMIT;
}

static bool reset_counts_for_loop(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_CPU_LOCKUP:
        return true;
#if CONFIG_ESP_IRIS_CRASH_LOOP_COUNT_POWER_FAULTS
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
        return true;
#endif
    default:
        return false;
    }
}

static bool current_image(uint32_t *out_address, uint8_t out_sha256[32])
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *description = esp_app_get_description();
    if (running == NULL || description == NULL) {
        return false;
    }
    *out_address = running->address;
    memcpy(out_sha256, description->app_elf_sha256, 32);
    return true;
}

#if CONFIG_ESP_IRIS_CRASH_LOOP_RECOVERY
static esp_err_t set_boot_address(uint32_t address)
{
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    esp_err_t result = ESP_ERR_NOT_FOUND;
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (partition != NULL && partition->address == address) {
            result = esp_ota_set_boot_partition(partition);
            break;
        }
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);
    return result;
}
#endif

static void runtime_from_state(iris_runtime_t *runtime,
                               const iris_crash_state_t *state)
{
    runtime->crash_count = state->count;
    runtime->crash_limit = state_limit(state);
    runtime->crash_origin_reset_reason = state->failure_reset_reason;
    runtime->crash_failed_app_address = state->failed_address;
    memcpy(runtime->crash_failed_firmware_sha256, state->failed_sha256,
           sizeof(runtime->crash_failed_firmware_sha256));
    runtime->crash_recovery_pending =
        (state->flags & IRIS_CRASH_FLAG_RECOVERY_PENDING) != 0;
    runtime->crash_loop_triggered =
        runtime->crash_recovery_pending ||
        (state->count >= state_limit(state) &&
         (state->flags & IRIS_CRASH_FLAG_FAILED_VALID) != 0);
}

static void clear_failure(iris_crash_state_t *state)
{
    state->flags &= ~(IRIS_CRASH_FLAG_FAILED_VALID |
                      IRIS_CRASH_FLAG_RECOVERY_PENDING);
    state->count = 0;
    state->failure_reset_reason = 0;
    state->failed_address = 0;
    memset(state->failed_sha256, 0, sizeof(state->failed_sha256));
}

esp_err_t iris_crash_recovery_probe(iris_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (runtime->crash_loop_initialized) {
        return runtime->crash_state_error;
    }
    runtime->crash_loop_initialized = true;
    runtime->crash_limit = CONFIG_ESP_IRIS_CRASH_LOOP_LIMIT;
    runtime->crash_state_error = ESP_OK;

#if !CONFIG_ESP_IRIS_CRASH_LOOP_TRACKING
    return ESP_OK;
#else
    SemaphoreHandle_t mutex = crash_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        runtime->crash_state_error = ESP_ERR_NO_MEM;
        return runtime->crash_state_error;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = state_open(&handle);
    if (err != ESP_OK) {
        xSemaphoreGive(mutex);
        runtime->crash_state_error = err;
        return err;
    }

    iris_crash_state_t state;
    state_init(&state);
    bool replaced = false;
    err = state_read(handle, &state, &replaced);
    uint32_t running_address = 0;
    uint8_t running_sha256[32] = {0};
    const bool have_running = current_image(&running_address, running_sha256);
    const esp_reset_reason_t reason = esp_reset_reason();

    if (err == ESP_OK) {
        runtime->previous_boot_planned =
            (state.flags & IRIS_CRASH_FLAG_PLANNED_RESTART) != 0 &&
            reason == ESP_RST_SW;
        state.flags &= ~IRIS_CRASH_FLAG_PLANNED_RESTART;
        state.planned_target_address = 0;

        if ((state.flags & IRIS_CRASH_FLAG_LAST_STARTED_VALID) != 0 &&
                reset_counts_for_loop(reason)) {
            if ((state.flags & IRIS_CRASH_FLAG_FAILED_VALID) != 0 &&
                    same_image(state.failed_address, state.failed_sha256,
                               state.last_started_address,
                               state.last_started_sha256)) {
                if (state.count < UINT32_MAX) {
                    ++state.count;
                }
            } else {
                state.count = 1;
            }
            state.flags |= IRIS_CRASH_FLAG_FAILED_VALID;
            state.failure_reset_reason = (uint32_t)reason;
            state.failed_address = state.last_started_address;
            memcpy(state.failed_sha256, state.last_started_sha256,
                   sizeof(state.failed_sha256));
        }

        if (have_running) {
            state.flags |= IRIS_CRASH_FLAG_LAST_STARTED_VALID;
            state.last_started_address = running_address;
            memcpy(state.last_started_sha256, running_sha256,
                   sizeof(state.last_started_sha256));
            if (CONFIG_ESP_IRIS_FIRMWARE_ROLE != 2) {
                state.limit = CONFIG_ESP_IRIS_CRASH_LOOP_LIMIT;
            }
        }

#if CONFIG_ESP_IRIS_CRASH_LOOP_RECOVERY
        const bool failed_image_running = have_running &&
            (state.flags & IRIS_CRASH_FLAG_FAILED_VALID) != 0 &&
            same_image(state.failed_address, state.failed_sha256,
                       running_address, running_sha256);
        if (failed_image_running &&
                state.count >= state_limit(&state)) {
            state.flags |= IRIS_CRASH_FLAG_RECOVERY_PENDING;
        }
#endif

        /* Always save after a valid read: this atomically consumes any
         * planned marker and records the identity of the image now starting. */
        err = state_write(handle, &state);
    }

    runtime_from_state(runtime, &state);
    if (CONFIG_ESP_IRIS_FIRMWARE_ROLE != 2 && state.count > 0) {
        runtime->crash_stable_deadline_us = esp_timer_get_time() +
            (int64_t)CONFIG_ESP_IRIS_CRASH_LOOP_STABLE_MS * 1000LL;
    }

#if CONFIG_ESP_IRIS_CRASH_LOOP_RECOVERY
    if (err == ESP_OK && have_running &&
            (state.flags & IRIS_CRASH_FLAG_RECOVERY_PENDING) != 0 &&
            same_image(state.failed_address, state.failed_sha256,
                       running_address, running_sha256)) {
        uint32_t recovery_address = 0;
        err = esp_iris_platform_select_recovery_target(&recovery_address);
        if (err == ESP_OK && recovery_address == running_address) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK) {
            state.flags |= IRIS_CRASH_FLAG_PLANNED_RESTART;
            state.planned_target_address = recovery_address;
            err = state_write(handle, &state);
        }
        if (err == ESP_OK) {
            err = set_boot_address(recovery_address);
        }
        if (err == ESP_OK) {
            nvs_close(handle);
            xSemaphoreGive(mutex);
            esp_restart();
            return ESP_OK;
        }
        state.flags &= ~IRIS_CRASH_FLAG_PLANNED_RESTART;
        state.planned_target_address = 0;
        (void)state_write(handle, &state);
    }
#endif

    nvs_close(handle);
    xSemaphoreGive(mutex);
    runtime->crash_state_error = err;
    runtime_from_state(runtime, &state);
    (void)replaced;
    return err;
#endif
}

esp_err_t iris_crash_recovery_mark_planned(iris_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->crash_loop_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
#if !CONFIG_ESP_IRIS_CRASH_LOOP_TRACKING
    runtime->previous_boot_planned = false;
    return ESP_OK;
#else
    SemaphoreHandle_t mutex = crash_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = state_open(&handle);
    iris_crash_state_t state;
    state_init(&state);
    bool replaced = false;
    if (err == ESP_OK) {
        err = state_read(handle, &state, &replaced);
    }
    if (err == ESP_OK) {
        const esp_partition_t *boot = esp_ota_get_boot_partition();
        state.flags |= IRIS_CRASH_FLAG_PLANNED_RESTART;
        state.planned_target_address = boot != NULL ? boot->address : 0;
        err = state_write(handle, &state);
        runtime_from_state(runtime, &state);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    xSemaphoreGive(mutex);
    runtime->crash_state_error = err;
    (void)replaced;
    return err;
#endif
}

esp_err_t iris_crash_recovery_mark_healthy(iris_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->crash_loop_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    runtime->crash_stable_deadline_us = 0;
    if (CONFIG_ESP_IRIS_FIRMWARE_ROLE == 2) {
        return ESP_OK;
    }
#if !CONFIG_ESP_IRIS_CRASH_LOOP_TRACKING
    return ESP_OK;
#else
    SemaphoreHandle_t mutex = crash_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = state_open(&handle);
    iris_crash_state_t state;
    bool replaced = false;
    if (err == ESP_OK) {
        err = state_read(handle, &state, &replaced);
    }
    if (err == ESP_OK) {
        clear_failure(&state);
        err = state_write(handle, &state);
        runtime_from_state(runtime, &state);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    xSemaphoreGive(mutex);
    runtime->crash_state_error = err;
    (void)replaced;
    return err;
#endif
}

esp_err_t iris_crash_recovery_reset(iris_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->crash_loop_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
#if !CONFIG_ESP_IRIS_CRASH_LOOP_TRACKING
    return ESP_OK;
#else
    SemaphoreHandle_t mutex = crash_mutex();
    if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = state_open(&handle);
    iris_crash_state_t state;
    bool replaced = false;
    if (err == ESP_OK) {
        err = state_read(handle, &state, &replaced);
    }
    if (err == ESP_OK) {
        clear_failure(&state);
        state.flags &= ~IRIS_CRASH_FLAG_PLANNED_RESTART;
        state.planned_target_address = 0;
        err = state_write(handle, &state);
        runtime_from_state(runtime, &state);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    xSemaphoreGive(mutex);
    runtime->crash_stable_deadline_us = 0;
    runtime->crash_state_error = err;
    (void)replaced;
    return err;
#endif
}

void iris_crash_recovery_poll(iris_runtime_t *runtime, int64_t now_us)
{
    if (runtime == NULL || runtime->crash_stable_deadline_us == 0 ||
            now_us < runtime->crash_stable_deadline_us) {
        return;
    }
    /* Consume the deadline before the NVS operation so a failing partition is
     * not hammered every 10 ms. The error remains visible in status. */
    runtime->crash_stable_deadline_us = 0;
    (void)iris_crash_recovery_mark_healthy(runtime);
}
