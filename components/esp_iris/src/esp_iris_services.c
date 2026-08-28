#include "esp_iris_internal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "psa/crypto.h"

#define IRIS_SERVICE_STATE_MAGIC 0x49525356U
#define IRIS_JOB_MAGIC 0x49524a42U
#define IRIS_RPC_HEADER_SIZE 12U
#define IRIS_RPC_RESPONSE_HEADER_SIZE 12U
#define IRIS_MEDIA_DESC_WIRE_SIZE 16U
#define IRIS_MEDIA_DATA_HEADER_SIZE 36U
#define IRIS_AUTH_TOKEN_BYTES 32U
#define IRIS_AUTH_CHALLENGE_BYTES 32U
#define IRIS_AUTH_NONCE_BYTES 16U
#define IRIS_AUTH_PROOF_BYTES 32U
#define IRIS_OTA_BEGIN_FIXED_SIZE 40U
#define IRIS_NVS_NAMESPACE "esp_iris"
#define IRIS_NVS_PAIR_TOKEN "pair_token"

typedef struct {
    bool used;
    uint16_t service_id;
    uint16_t method_id;
    esp_iris_rpc_handler_t handler;
    void *user_ctx;
} iris_rpc_entry_t;

struct esp_iris_job {
    uint32_t magic;
    esp_iris_job_info_t info;
    esp_iris_job_cancel_fn cancel;
    void *user_ctx;
    bool event_pending;
};

typedef struct {
    bool active;
    bool pending;
    bool pull;
    esp_iris_media_desc_t description;
    esp_iris_media_desc_t frame_description;
    uint32_t frame_id;
    uint32_t stream_id;
    uint32_t pull_frame_id;
    uint32_t total_size;
    uint32_t offset;
    uint16_t flags;
    uint16_t fps;
    uint64_t monotonic_us;
    int64_t next_frame_us;
    uint32_t dropped;
    uint32_t credit;
    size_t size;
    uint8_t *data;
} iris_media_slot_t;

typedef struct {
    bool active;
    esp_iris_media_desc_t description;
    uint32_t total_size;
    uint32_t stream_id;
} iris_capture_t;

#if CONFIG_ESP_IRIS_OTA
typedef struct {
    bool active;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    uint32_t total_size;
    uint32_t received;
    uint8_t expected_sha256[32];
    psa_hash_operation_t hash;
    bool hash_active;
    char project_name[33];
    char version[33];
    esp_iris_job_handle_t job;
} iris_ota_t;
#endif

typedef struct {
    uint32_t magic;
    iris_rpc_entry_t rpc[CONFIG_ESP_IRIS_MAX_RPC_HANDLERS];
    uint8_t rpc_response[CONFIG_ESP_IRIS_RPC_BODY_BYTES];
    struct esp_iris_job jobs[CONFIG_ESP_IRIS_MAX_JOBS];
    uint32_t next_job_id;
    uint32_t last_rpc_request_id;
    esp_iris_screen_backend_t screen;
    iris_capture_t capture;
    iris_media_slot_t media[3];
    uint8_t auth_token[IRIS_AUTH_TOKEN_BYTES];
    uint8_t auth_challenge[IRIS_AUTH_CHALLENGE_BYTES];
    bool auth_token_ready;
    int64_t auth_retry_after_us;
    uint32_t restart_delay_ms;
    int64_t restart_at_us;
    bool restart_pending;
    uint32_t allocated_bytes;
    uint32_t media_users;
#if CONFIG_ESP_IRIS_OTA
    iris_ota_t ota;
#endif
} iris_service_state_t;

static iris_service_state_t *s_services;
static portMUX_TYPE s_services_lock = portMUX_INITIALIZER_UNLOCKED;

static iris_service_state_t *service_state(bool create)
{
    taskENTER_CRITICAL(&s_services_lock);
    iris_service_state_t *state = s_services;
    taskEXIT_CRITICAL(&s_services_lock);
    if (state != NULL || !create) {
        return state;
    }
    state = heap_caps_calloc(1, sizeof(*state), MALLOC_CAP_INTERNAL);
    if (state == NULL) {
        return NULL;
    }
    state->magic = IRIS_SERVICE_STATE_MAGIC;
    state->allocated_bytes = sizeof(*state);
    taskENTER_CRITICAL(&s_services_lock);
    if (s_services == NULL) {
        s_services = state;
    } else {
        free(state);
        state = s_services;
    }
    taskEXIT_CRITICAL(&s_services_lock);
    return state;
}

static bool valid_state(const iris_service_state_t *state)
{
    return state != NULL && state->magic == IRIS_SERVICE_STATE_MAGIC;
}

static void maybe_release_state(iris_service_state_t *state)
{
    bool release = false;
    taskENTER_CRITICAL(&s_services_lock);
    if (s_services != state || !valid_state(state) ||
        state->auth_token_ready || state->screen.begin != NULL ||
        state->capture.active || state->restart_pending ||
        state->media_users != 0) {
        taskEXIT_CRITICAL(&s_services_lock);
        return;
    }
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_RPC_HANDLERS; ++i) {
        if (state->rpc[i].used) {
            taskEXIT_CRITICAL(&s_services_lock);
            return;
        }
    }
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_JOBS; ++i) {
        if (state->jobs[i].magic == IRIS_JOB_MAGIC) {
            taskEXIT_CRITICAL(&s_services_lock);
            return;
        }
    }
    for (size_t i = 0; i < 3; ++i) {
        if (state->media[i].data != NULL) {
            taskEXIT_CRITICAL(&s_services_lock);
            return;
        }
    }
    s_services = NULL;
    state->magic = 0;
    release = true;
    taskEXIT_CRITICAL(&s_services_lock);
    if (release) {
        free(state);
    }
}

static iris_service_state_t *media_state_acquire(void)
{
    taskENTER_CRITICAL(&s_services_lock);
    iris_service_state_t *state = s_services;
    if (valid_state(state)) {
        ++state->media_users;
    } else {
        state = NULL;
    }
    taskEXIT_CRITICAL(&s_services_lock);
    return state;
}

static void media_state_release(iris_service_state_t *state)
{
    taskENTER_CRITICAL(&s_services_lock);
    if (valid_state(state) && state->media_users > 0) {
        --state->media_users;
    }
    taskEXIT_CRITICAL(&s_services_lock);
    maybe_release_state(state);
}

static void media_desc_decode(const uint8_t *payload,
                              esp_iris_media_desc_t *description)
{
    description->x = iris_get_le16(payload);
    description->y = iris_get_le16(payload + 2);
    description->width = iris_get_le16(payload + 4);
    description->height = iris_get_le16(payload + 6);
    description->stride = iris_get_le32(payload + 8);
    description->format = iris_get_le16(payload + 12);
    description->quality = iris_get_le16(payload + 14);
}

static void media_desc_encode(uint8_t *payload,
                              const esp_iris_media_desc_t *description)
{
    iris_put_le16(payload, description->x);
    iris_put_le16(payload + 2, description->y);
    iris_put_le16(payload + 4, description->width);
    iris_put_le16(payload + 6, description->height);
    iris_put_le32(payload + 8, description->stride);
    iris_put_le16(payload + 12, description->format);
    iris_put_le16(payload + 14, description->quality);
}

static int media_index(uint8_t channel)
{
    if (channel < ESP_IRIS_CHANNEL_SCREEN ||
        channel > ESP_IRIS_CHANNEL_AUDIO) {
        return -1;
    }
    return channel - ESP_IRIS_CHANNEL_SCREEN;
}

static esp_err_t ensure_media_buffer(iris_service_state_t *state,
                                     iris_media_slot_t *slot)
{
    if (slot->data != NULL) {
        return ESP_OK;
    }
    uint8_t *data = heap_caps_malloc(CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES,
                                     MALLOC_CAP_INTERNAL);
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_services_lock);
    slot->data = data;
    state->allocated_bytes += CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES;
    taskEXIT_CRITICAL(&s_services_lock);
    return ESP_OK;
}

static void release_media_buffer(iris_service_state_t *state,
                                 iris_media_slot_t *slot)
{
    taskENTER_CRITICAL(&s_services_lock);
    uint8_t *data = slot->data;
    slot->data = NULL;
    slot->active = false;
    slot->pending = false;
    slot->pull = false;
    slot->credit = 0;
    slot->size = 0;
    slot->stream_id = 0;
    slot->total_size = 0;
    slot->offset = 0;
    if (data != NULL) {
        state->allocated_bytes -= CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES;
    }
    taskEXIT_CRITICAL(&s_services_lock);
    free(data);
}

static bool job_valid(iris_service_state_t *state,
                      esp_iris_job_handle_t job)
{
    if (!valid_state(state) || job == NULL || job->magic != IRIS_JOB_MAGIC) {
        return false;
    }
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_JOBS; ++i) {
        if (&state->jobs[i] == job) {
            return true;
        }
    }
    return false;
}

static void job_encode(uint8_t out[16], const esp_iris_job_info_t *info)
{
    iris_put_le32(out, info->id);
    iris_put_le16(out + 4, info->kind);
    out[6] = (uint8_t)info->state;
    out[7] = info->cancel_requested ? 1U : 0U;
    iris_put_le16(out + 8, info->progress_permille);
    iris_put_le16(out + 10, 0);
    iris_put_le32(out + 12, (uint32_t)info->result);
}

esp_err_t esp_iris_rpc_register(uint16_t service_id, uint16_t method_id,
                                esp_iris_rpc_handler_t handler,
                                void *user_ctx)
{
    if (service_id == 0 || method_id == 0 || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    iris_service_state_t *state = service_state(true);
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_services_lock);
    iris_rpc_entry_t *free_entry = NULL;
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_RPC_HANDLERS; ++i) {
        iris_rpc_entry_t *entry = &state->rpc[i];
        if (entry->used && entry->service_id == service_id &&
            entry->method_id == method_id) {
            taskEXIT_CRITICAL(&s_services_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (!entry->used && free_entry == NULL) {
            free_entry = entry;
        }
    }
    if (free_entry != NULL) {
        *free_entry = (iris_rpc_entry_t) {
            .used = true,
            .service_id = service_id,
            .method_id = method_id,
            .handler = handler,
            .user_ctx = user_ctx,
        };
    }
    taskEXIT_CRITICAL(&s_services_lock);
    return free_entry == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t esp_iris_rpc_unregister(uint16_t service_id, uint16_t method_id)
{
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state)) {
        return ESP_ERR_NOT_FOUND;
    }
    taskENTER_CRITICAL(&s_services_lock);
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_RPC_HANDLERS; ++i) {
        iris_rpc_entry_t *entry = &state->rpc[i];
        if (entry->used && entry->service_id == service_id &&
            entry->method_id == method_id) {
            memset(entry, 0, sizeof(*entry));
            taskEXIT_CRITICAL(&s_services_lock);
            maybe_release_state(state);
            return ESP_OK;
        }
    }
    taskEXIT_CRITICAL(&s_services_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t esp_iris_job_create(uint16_t kind, esp_iris_job_cancel_fn cancel,
                              void *user_ctx,
                              esp_iris_job_handle_t *out_job)
{
    if (kind == 0 || out_job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    iris_service_state_t *state = service_state(true);
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_services_lock);
    struct esp_iris_job *slot = NULL;
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_JOBS; ++i) {
        if (state->jobs[i].magic != IRIS_JOB_MAGIC ||
            state->jobs[i].info.state >= ESP_IRIS_JOB_SUCCEEDED) {
            slot = &state->jobs[i];
            break;
        }
    }
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
        slot->magic = IRIS_JOB_MAGIC;
        do {
            slot->info.id = ++state->next_job_id;
        } while (slot->info.id == 0);
        slot->info.kind = kind;
        slot->info.state = ESP_IRIS_JOB_RUNNING;
        slot->cancel = cancel;
        slot->user_ctx = user_ctx;
        slot->event_pending = true;
        *out_job = slot;
    }
    taskEXIT_CRITICAL(&s_services_lock);
    if (slot != NULL && g_iris.task != NULL) {
        xTaskNotifyGive(g_iris.task);
    }
    return slot == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t esp_iris_job_update(esp_iris_job_handle_t job,
                              uint16_t progress_permille)
{
    iris_service_state_t *state = service_state(false);
    if (!job_valid(state, job) || progress_permille > 1000) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_services_lock);
    if (job->info.state != ESP_IRIS_JOB_RUNNING) {
        taskEXIT_CRITICAL(&s_services_lock);
        return ESP_ERR_INVALID_STATE;
    }
    job->info.progress_permille = progress_permille;
    job->event_pending = true;
    taskEXIT_CRITICAL(&s_services_lock);
    if (g_iris.task != NULL) {
        xTaskNotifyGive(g_iris.task);
    }
    return ESP_OK;
}

esp_err_t esp_iris_job_finish(esp_iris_job_handle_t job, esp_err_t result)
{
    iris_service_state_t *state = service_state(false);
    if (!job_valid(state, job)) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_services_lock);
    if (job->info.state != ESP_IRIS_JOB_RUNNING) {
        taskEXIT_CRITICAL(&s_services_lock);
        return ESP_ERR_INVALID_STATE;
    }
    job->info.result = result;
    job->info.progress_permille = result == ESP_OK ? 1000 : job->info.progress_permille;
    job->info.state = job->info.cancel_requested
        ? ESP_IRIS_JOB_CANCELLED
        : (result == ESP_OK ? ESP_IRIS_JOB_SUCCEEDED : ESP_IRIS_JOB_FAILED);
    job->event_pending = true;
    taskEXIT_CRITICAL(&s_services_lock);
    if (g_iris.task != NULL) {
        xTaskNotifyGive(g_iris.task);
    }
    return ESP_OK;
}

bool esp_iris_job_cancel_requested(esp_iris_job_handle_t job)
{
    iris_service_state_t *state = service_state(false);
    if (!job_valid(state, job)) {
        return false;
    }
    taskENTER_CRITICAL(&s_services_lock);
    bool requested = job->info.cancel_requested;
    taskEXIT_CRITICAL(&s_services_lock);
    return requested;
}

esp_err_t esp_iris_job_get_info(esp_iris_job_handle_t job,
                                esp_iris_job_info_t *out_info)
{
    iris_service_state_t *state = service_state(false);
    if (!job_valid(state, job) || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_services_lock);
    *out_info = job->info;
    taskEXIT_CRITICAL(&s_services_lock);
    return ESP_OK;
}

esp_err_t esp_iris_screen_register(const esp_iris_screen_backend_t *backend)
{
    if (backend == NULL || backend->begin == NULL || backend->read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    iris_service_state_t *state = service_state(true);
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_services_lock);
    if (state->screen.begin != NULL) {
        taskEXIT_CRITICAL(&s_services_lock);
        return ESP_ERR_INVALID_STATE;
    }
    state->screen = *backend;
    taskEXIT_CRITICAL(&s_services_lock);
    return ESP_OK;
}

esp_err_t esp_iris_screen_unregister(void *user_ctx)
{
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state)) {
        return ESP_ERR_NOT_FOUND;
    }
    taskENTER_CRITICAL(&s_services_lock);
    if (state->screen.begin == NULL || state->screen.user_ctx != user_ctx ||
        state->capture.active || state->media[0].active) {
        taskEXIT_CRITICAL(&s_services_lock);
        return state->capture.active || state->media[0].active
            ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    memset(&state->screen, 0, sizeof(state->screen));
    taskEXIT_CRITICAL(&s_services_lock);
    maybe_release_state(state);
    return ESP_OK;
}

esp_err_t esp_iris_media_submit(esp_iris_channel_t channel,
                                const esp_iris_media_desc_t *description,
                                uint32_t frame_id, uint16_t flags,
                                const void *data, size_t size)
{
    const int index = media_index((uint8_t)channel);
    if (index < 0 || description == NULL || data == NULL || size == 0 ||
        size > CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    iris_service_state_t *state = media_state_acquire();
    if (!valid_state(state)) {
        return ESP_ERR_INVALID_STATE;
    }
    iris_media_slot_t *slot = &state->media[index];
    taskENTER_CRITICAL(&s_services_lock);
    if (!slot->active || slot->data == NULL || slot->pull) {
        taskEXIT_CRITICAL(&s_services_lock);
        media_state_release(state);
        return ESP_ERR_INVALID_STATE;
    }
    if (slot->pending) {
        ++slot->dropped;
    }
    slot->description = *description;
    slot->frame_id = frame_id;
    slot->flags = flags;
    slot->monotonic_us = (uint64_t)esp_timer_get_time();
    memcpy(slot->data, data, size);
    slot->size = size;
    slot->pending = true;
    taskEXIT_CRITICAL(&s_services_lock);
    media_state_release(state);
    if (g_iris.task != NULL) {
        xTaskNotifyGive(g_iris.task);
    }
    return ESP_OK;
}

bool esp_iris_media_is_streaming(esp_iris_channel_t channel)
{
    const int index = media_index((uint8_t)channel);
    if (index < 0) {
        return false;
    }
    iris_service_state_t *state = media_state_acquire();
    if (!valid_state(state)) {
        return false;
    }
    taskENTER_CRITICAL(&s_services_lock);
    bool active = state->media[index].active;
    taskEXIT_CRITICAL(&s_services_lock);
    media_state_release(state);
    return active;
}

static esp_err_t pairing_load_or_create(iris_service_state_t *state,
                                        bool rotate)
{
#if CONFIG_ESP_IRIS_TCP_PAIRING
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IRIS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = sizeof(state->auth_token);
    if (!rotate) {
        err = nvs_get_blob(handle, IRIS_NVS_PAIR_TOKEN,
                           state->auth_token, &size);
    } else {
        err = ESP_ERR_NVS_NOT_FOUND;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        esp_fill_random(state->auth_token, sizeof(state->auth_token));
        err = nvs_set_blob(handle, IRIS_NVS_PAIR_TOKEN, state->auth_token,
                           sizeof(state->auth_token));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    } else if (err == ESP_OK && size != sizeof(state->auth_token)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(handle);
    state->auth_token_ready = err == ESP_OK;
    return err;
#else
    (void)state;
    (void)rotate;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void token_format(const uint8_t token[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[token[i] >> 4];
        out[i * 2 + 1] = hex[token[i] & 0x0fU];
    }
    out[64] = '\0';
}

#if CONFIG_ESP_IRIS_TCP_PAIRING
static int token_hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}
#endif

esp_err_t esp_iris_pairing_token_get(char out[65])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state) || !state->auth_token_ready) {
        out[0] = '\0';
        return ESP_ERR_NOT_SUPPORTED;
    }
    token_format(state->auth_token, out);
    return ESP_OK;
}

esp_err_t esp_iris_pairing_token_rotate(char out[65])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    iris_service_state_t *state = service_state(true);
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = pairing_load_or_create(state, true);
    if (err == ESP_OK) {
        token_format(state->auth_token, out);
    } else {
        out[0] = '\0';
    }
    return err;
}

esp_err_t esp_iris_pairing_token_set(const char token[65])
{
#if CONFIG_ESP_IRIS_TCP_PAIRING
    if (token == NULL || strnlen(token, 65) != 64) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t parsed[IRIS_AUTH_TOKEN_BYTES];
    for (size_t i = 0; i < sizeof(parsed); ++i) {
        const int high = token_hex_nibble(token[i * 2]);
        const int low = token_hex_nibble(token[i * 2 + 1]);
        if (high < 0 || low < 0) {
            memset(parsed, 0, sizeof(parsed));
            return ESP_ERR_INVALID_ARG;
        }
        parsed[i] = (uint8_t)((high << 4) | low);
    }
    iris_service_state_t *state = service_state(true);
    if (state == NULL) {
        memset(parsed, 0, sizeof(parsed));
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IRIS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, IRIS_NVS_PAIR_TOKEN, parsed,
                           sizeof(parsed));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_services_lock);
        memcpy(state->auth_token, parsed, sizeof(parsed));
        state->auth_token_ready = true;
        taskEXIT_CRITICAL(&s_services_lock);
    }
    memset(parsed, 0, sizeof(parsed));
    return err;
#else
    (void)token;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t iris_services_init(iris_runtime_t *runtime)
{
    esp_err_t err = iris_files_init(runtime);
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_ESP_IRIS_TCP_PAIRING
    iris_service_state_t *state = service_state(true);
    return state == NULL ? ESP_ERR_NO_MEM : pairing_load_or_create(state, false);
#else
    return ESP_OK;
#endif
}

static void ota_abort(iris_service_state_t *state, esp_err_t result)
{
#if CONFIG_ESP_IRIS_OTA
    if (!state->ota.active) {
        return;
    }
    (void)esp_ota_abort(state->ota.handle);
    if (state->ota.hash_active) {
        (void)psa_hash_abort(&state->ota.hash);
    }
    if (state->ota.job != NULL) {
        (void)esp_iris_job_finish(state->ota.job, result);
    }
    memset(&state->ota, 0, sizeof(state->ota));
#else
    (void)state;
    (void)result;
#endif
}

void iris_services_deinit(iris_runtime_t *runtime)
{
    (void)runtime;
    iris_files_deinit();
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state)) {
        return;
    }
    ota_abort(state, ESP_ERR_INVALID_STATE);
    if (state->capture.active && state->screen.end != NULL) {
        state->screen.end(state->screen.user_ctx);
    }
    state->capture.active = false;
    for (size_t i = 0; i < 3; ++i) {
        if (i == 0 && state->media[i].pull && state->screen.end != NULL) {
            state->screen.end(state->screen.user_ctx);
        }
        release_media_buffer(state, &state->media[i]);
    }
    memset(state->jobs, 0, sizeof(state->jobs));
    state->restart_pending = false;
    maybe_release_state(state);
}

void iris_services_session_begin(iris_runtime_t *runtime)
{
    (void)runtime;
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state)) {
        return;
    }
    state->last_rpc_request_id = 0;
#if CONFIG_ESP_IRIS_TCP_PAIRING
    esp_fill_random(state->auth_challenge, sizeof(state->auth_challenge));
#endif
}

void iris_services_session_end(iris_runtime_t *runtime)
{
    iris_files_session_end(runtime->session_id);
    iris_system_update_session_end();
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state)) {
        return;
    }
    ota_abort(state, ESP_ERR_INVALID_STATE);
    if (state->capture.active && state->screen.end != NULL) {
        state->screen.end(state->screen.user_ctx);
    }
    state->capture.active = false;
    for (size_t i = 0; i < 3; ++i) {
        if (i == 0 && state->media[i].pull && state->screen.end != NULL) {
            state->screen.end(state->screen.user_ctx);
        }
        release_media_buffer(state, &state->media[i]);
    }
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_JOBS; ++i) {
        struct esp_iris_job *job = &state->jobs[i];
        if (job->magic == IRIS_JOB_MAGIC &&
            job->info.state == ESP_IRIS_JOB_RUNNING) {
            job->info.cancel_requested = true;
            if (job->cancel != NULL) {
                job->cancel(job->user_ctx);
            }
            (void)esp_iris_job_finish(job, ESP_ERR_INVALID_STATE);
        }
    }
    maybe_release_state(state);
}

uint64_t iris_services_capabilities(void)
{
    uint64_t result = ESP_IRIS_CAP_RPC | ESP_IRIS_CAP_JOBS |
                      ESP_IRIS_CAP_SCREEN | ESP_IRIS_CAP_IMAGE |
                      ESP_IRIS_CAP_AUDIO | ESP_IRIS_CAP_MIRROR |
                      iris_files_capabilities() |
                      iris_system_inventory_capabilities() |
                      iris_system_update_capabilities();
#if CONFIG_ESP_IRIS_OTA
    if (esp_ota_get_next_update_partition(NULL) != NULL) {
        result |= ESP_IRIS_CAP_OTA;
    }
#endif
#if CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH
    result |= ESP_IRIS_CAP_OTA_PROJECT_NAME_MATCH;
#endif
#if CONFIG_ESP_IRIS_TCP_PAIRING
    if (iris_transport_kind() == ESP_IRIS_TRANSPORT_KIND_TCP) {
        result |= ESP_IRIS_CAP_AUTH;
    }
#endif
    return result;
}

uint8_t iris_services_auth_mode(void)
{
#if CONFIG_ESP_IRIS_TCP_PAIRING
    if (iris_transport_kind() == ESP_IRIS_TRANSPORT_KIND_TCP) {
        return 1;
    }
#endif
    return 0;
}

const uint8_t *iris_services_auth_challenge(size_t *size)
{
    iris_service_state_t *state = service_state(false);
    if (size != NULL) {
        *size = 0;
    }
#if CONFIG_ESP_IRIS_TCP_PAIRING
    if (iris_transport_kind() == ESP_IRIS_TRANSPORT_KIND_TCP &&
            valid_state(state) && state->auth_token_ready) {
        if (size != NULL) {
            *size = sizeof(state->auth_challenge);
        }
        return state->auth_challenge;
    }
#else
    (void)state;
#endif
    return NULL;
}

#if CONFIG_ESP_IRIS_TCP_PAIRING || CONFIG_ESP_IRIS_OTA
static bool constant_time_equal(const uint8_t *left, const uint8_t *right,
                                size_t size)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}
#endif

esp_err_t iris_services_authenticate(const iris_runtime_t *runtime,
                                     const uint8_t *payload, size_t size)
{
#if CONFIG_ESP_IRIS_TCP_PAIRING
    if (iris_transport_kind() != ESP_IRIS_TRANSPORT_KIND_TCP) {
        return size == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state) || !state->auth_token_ready ||
        payload == NULL ||
        size != IRIS_AUTH_NONCE_BYTES + IRIS_AUTH_PROOF_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t now = esp_timer_get_time();
    if (now < state->auth_retry_after_us) {
        return ESP_ERR_TIMEOUT;
    }
    static const uint8_t label[] = "ESP-Iris-auth-v1";
    uint8_t message[sizeof(label) - 1 + 16 + 8 + 4 +
                    IRIS_AUTH_CHALLENGE_BYTES + IRIS_AUTH_NONCE_BYTES];
    size_t offset = 0;
    memcpy(message + offset, label, sizeof(label) - 1);
    offset += sizeof(label) - 1;
    memcpy(message + offset, runtime->device_id, sizeof(runtime->device_id));
    offset += sizeof(runtime->device_id);
    iris_put_le64(message + offset, runtime->boot_id);
    offset += 8;
    iris_put_le32(message + offset, runtime->session_id);
    offset += 4;
    memcpy(message + offset, state->auth_challenge,
           sizeof(state->auth_challenge));
    offset += sizeof(state->auth_challenge);
    memcpy(message + offset, payload, IRIS_AUTH_NONCE_BYTES);
    offset += IRIS_AUTH_NONCE_BYTES;
    uint8_t expected[IRIS_AUTH_PROOF_BYTES];
    size_t expected_size = 0;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    const psa_algorithm_t algorithm = PSA_ALG_HMAC(PSA_ALG_SHA_256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, algorithm);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, IRIS_AUTH_TOKEN_BYTES * 8);
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
    psa_key_id_t key_id = 0;
    psa_status_t result = psa_crypto_init();
    if (result == PSA_SUCCESS) {
        result = psa_import_key(&attributes, state->auth_token,
                                sizeof(state->auth_token), &key_id);
    }
    if (result == PSA_SUCCESS) {
        result = psa_mac_compute(key_id, algorithm, message, offset,
                                 expected, sizeof(expected),
                                 &expected_size);
    }
    if (key_id != 0) {
        (void)psa_destroy_key(key_id);
    }
    psa_reset_key_attributes(&attributes);
    if (result != PSA_SUCCESS || expected_size != sizeof(expected) ||
        !constant_time_equal(expected, payload + IRIS_AUTH_NONCE_BYTES,
                             sizeof(expected))) {
        state->auth_retry_after_us = now +
            (int64_t)CONFIG_ESP_IRIS_AUTH_FAILURE_DELAY_MS * 1000;
        return ESP_ERR_INVALID_CRC;
    }
    state->auth_retry_after_us = 0;
    return ESP_OK;
#else
    (void)runtime;
    return size == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
#endif
}

bool iris_services_credit(uint8_t channel, uint32_t amount)
{
    const int index = media_index(channel);
    iris_service_state_t *state = service_state(false);
    if (index < 0 || !valid_state(state)) {
        return false;
    }
    taskENTER_CRITICAL(&s_services_lock);
    iris_media_slot_t *slot = &state->media[index];
    slot->credit = UINT32_MAX - slot->credit < amount
        ? UINT32_MAX : slot->credit + amount;
    taskEXIT_CRITICAL(&s_services_lock);
    return true;
}

static bool handle_rpc(iris_runtime_t *runtime,
                       const iris_decoded_frame_t *frame,
                       uint64_t received_us)
{
    const esp_iris_wire_header_t *header = &frame->header;
    if (header->type != ESP_IRIS_CONTROL_REQUEST) {
        return false;
    }
    if (header->payload_size < IRIS_RPC_HEADER_SIZE) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_INVALID_SIZE, header->channel,
                               header->type);
        return true;
    }
    const uint16_t service_id = iris_get_le16(frame->payload);
    const uint16_t method_id = iris_get_le16(frame->payload + 2);
    const uint32_t deadline_ms = iris_get_le32(frame->payload + 4);
    const uint16_t body_size = iris_get_le16(frame->payload + 8);
    if (frame->payload[10] != 0 || frame->payload[11] != 0 ||
        body_size > CONFIG_ESP_IRIS_RPC_BODY_BYTES ||
        header->payload_size != IRIS_RPC_HEADER_SIZE + body_size) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_INVALID_SIZE, header->channel,
                               header->type);
        return true;
    }
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state) || state->last_rpc_request_id == header->request_id) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_INVALID_STATE, header->channel,
                               header->type);
        return true;
    }
    state->last_rpc_request_id = header->request_id;
    esp_iris_rpc_handler_t handler = NULL;
    void *user_ctx = NULL;
    taskENTER_CRITICAL(&s_services_lock);
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_RPC_HANDLERS; ++i) {
        if (state->rpc[i].used && state->rpc[i].service_id == service_id &&
            state->rpc[i].method_id == method_id) {
            handler = state->rpc[i].handler;
            user_ctx = state->rpc[i].user_ctx;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_services_lock);
    if (handler == NULL) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_NOT_FOUND, header->channel,
                               header->type);
        return true;
    }
    esp_iris_rpc_request_t request = {
        .service_id = service_id,
        .method_id = method_id,
        .request_id = header->request_id,
        .deadline_ms = deadline_ms,
        .payload = frame->payload + IRIS_RPC_HEADER_SIZE,
        .payload_size = body_size,
    };
    size_t response_size = 0;
    esp_err_t err = handler(&request, state->rpc_response,
                            CONFIG_ESP_IRIS_RPC_BODY_BYTES,
                            &response_size, user_ctx);
    const uint64_t finished_us = (uint64_t)esp_timer_get_time();
    const uint64_t elapsed_us = finished_us >= received_us
        ? finished_us - received_us : UINT64_MAX;
    if (response_size > CONFIG_ESP_IRIS_RPC_BODY_BYTES) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (deadline_ms != 0 && elapsed_us > (uint64_t)deadline_ms * 1000U) {
        err = ESP_ERR_TIMEOUT;
        response_size = 0;
    }
    iris_put_le16(runtime->rx_wire, service_id);
    iris_put_le16(runtime->rx_wire + 2, method_id);
    iris_put_le32(runtime->rx_wire + 4, (uint32_t)err);
    iris_put_le16(runtime->rx_wire + 8, (uint16_t)response_size);
    iris_put_le16(runtime->rx_wire + 10, 0);
    if (response_size > 0) {
        memcpy(runtime->rx_wire + IRIS_RPC_RESPONSE_HEADER_SIZE,
               state->rpc_response, response_size);
    }
    (void)iris_queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                           ESP_IRIS_CONTROL_RESPONSE,
                           ESP_IRIS_FLAG_RESPONSE |
                               (err == ESP_OK ? 0 : ESP_IRIS_FLAG_ERROR),
                           header->request_id, 0, runtime->rx_wire,
                           IRIS_RPC_RESPONSE_HEADER_SIZE + response_size);
    return true;
}

static struct esp_iris_job *find_job(iris_service_state_t *state,
                                     uint32_t id)
{
    if (!valid_state(state)) {
        return NULL;
    }
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_JOBS; ++i) {
        if (state->jobs[i].magic == IRIS_JOB_MAGIC &&
            state->jobs[i].info.id == id) {
            return &state->jobs[i];
        }
    }
    return NULL;
}

static bool handle_job(iris_runtime_t *runtime,
                       const iris_decoded_frame_t *frame)
{
    const esp_iris_wire_header_t *header = &frame->header;
    if (header->type != ESP_IRIS_CONTROL_JOB_QUERY &&
        header->type != ESP_IRIS_CONTROL_CANCEL) {
        return false;
    }
    if (header->payload_size != 4) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_INVALID_SIZE, header->channel,
                               header->type);
        return true;
    }
    iris_service_state_t *state = service_state(false);
    taskENTER_CRITICAL(&s_services_lock);
    struct esp_iris_job *job = find_job(state, iris_get_le32(frame->payload));
    if (job == NULL) {
        taskEXIT_CRITICAL(&s_services_lock);
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_NOT_FOUND, header->channel,
                               header->type);
        return true;
    }
    esp_iris_job_cancel_fn cancel = NULL;
    void *cancel_ctx = NULL;
    if (header->type == ESP_IRIS_CONTROL_CANCEL &&
        job->info.state == ESP_IRIS_JOB_RUNNING) {
        job->info.cancel_requested = true;
        job->event_pending = true;
        cancel = job->cancel;
        cancel_ctx = job->user_ctx;
    }
    uint8_t payload[16];
    job_encode(payload, &job->info);
    taskEXIT_CRITICAL(&s_services_lock);
    if (cancel != NULL) {
        cancel(cancel_ctx);
    }
    (void)iris_queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                           ESP_IRIS_CONTROL_JOB_STATUS,
                           ESP_IRIS_FLAG_RESPONSE, header->request_id, 0,
                           payload, sizeof(payload));
    return true;
}

static bool handle_restart(iris_runtime_t *runtime,
                           const iris_decoded_frame_t *frame)
{
    const esp_iris_wire_header_t *header = &frame->header;
    if (header->type != ESP_IRIS_CONTROL_RESTART) {
        return false;
    }
    if (header->payload_size != 4) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_INVALID_SIZE, header->channel,
                               header->type);
        return true;
    }
    uint32_t delay_ms = iris_get_le32(frame->payload);
    if (delay_ms < 100) {
        delay_ms = 100;
    }
    if (delay_ms > 60000) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_INVALID_ARG, header->channel,
                               header->type);
        return true;
    }
    iris_service_state_t *state = service_state(true);
    if (state == NULL) {
        (void)iris_queue_error(runtime, header->request_id, ESP_ERR_NO_MEM,
                               header->channel, header->type);
        return true;
    }
    esp_err_t err = esp_iris_mark_planned_restart();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        state->restart_delay_ms = delay_ms;
        state->restart_at_us = esp_timer_get_time() +
                               (int64_t)delay_ms * 1000;
        state->restart_pending = true;
        uint8_t response[4];
        iris_put_le32(response, delay_ms);
        (void)iris_queue_frame(runtime, ESP_IRIS_CHANNEL_CONTROL,
                               ESP_IRIS_CONTROL_RESTART,
                               ESP_IRIS_FLAG_RESPONSE,
                               header->request_id, 0,
                               response, sizeof(response));
    } else {
        (void)iris_queue_error(runtime, header->request_id, err,
                               header->channel, header->type);
    }
    return true;
}

static bool handle_media(iris_runtime_t *runtime,
                         const iris_decoded_frame_t *frame)
{
    const esp_iris_wire_header_t *header = &frame->header;
    const int index = media_index(header->channel);
    if (index < 0) {
        return false;
    }
    if ((header->type == ESP_IRIS_MEDIA_OPEN &&
         header->channel == ESP_IRIS_CHANNEL_SCREEN &&
         header->payload_size != IRIS_MEDIA_DESC_WIRE_SIZE) ||
        (header->type == ESP_IRIS_MEDIA_MIRROR_START &&
         header->payload_size != 20)) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_INVALID_SIZE,
                               header->channel, header->type);
        return true;
    }
    const bool create = header->type == ESP_IRIS_MEDIA_MIRROR_START;
    iris_service_state_t *state = service_state(create);
    if (state == NULL) {
        if (header->type == ESP_IRIS_MEDIA_MIRROR_STOP) {
            (void)iris_queue_frame(runtime, header->channel,
                                   ESP_IRIS_MEDIA_MIRROR_STATE,
                                   ESP_IRIS_FLAG_RESPONSE |
                                       ESP_IRIS_FLAG_STREAM_END,
                                   header->request_id, header->stream_id,
                                   NULL, 0);
        } else {
            const esp_err_t err = create ? ESP_ERR_NO_MEM :
                (header->type == ESP_IRIS_MEDIA_OPEN
                    ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE);
            (void)iris_queue_error(runtime, header->request_id, err,
                                   header->channel, header->type);
        }
        return true;
    }
    if (header->type == ESP_IRIS_MEDIA_OPEN &&
        header->channel == ESP_IRIS_CHANNEL_SCREEN) {
        if (state->screen.begin == NULL || state->capture.active ||
            state->media[0].active) {
            (void)iris_queue_error(runtime, header->request_id,
                state->screen.begin == NULL ? ESP_ERR_NOT_SUPPORTED
                                            : ESP_ERR_INVALID_STATE,
                header->channel, header->type);
            return true;
        }
        esp_iris_media_desc_t requested;
        esp_iris_media_desc_t actual;
        media_desc_decode(frame->payload, &requested);
        uint32_t total_size = 0;
        esp_err_t err = state->screen.begin(&requested, &actual, &total_size,
                                            state->screen.user_ctx);
        if (err != ESP_OK || total_size == 0) {
            (void)iris_queue_error(runtime, header->request_id,
                                   err == ESP_OK ? ESP_ERR_INVALID_SIZE : err,
                                   header->channel, header->type);
            return true;
        }
        state->capture = (iris_capture_t) {
            .active = true,
            .description = actual,
            .total_size = total_size,
            .stream_id = header->request_id != 0 ? header->request_id : 1,
        };
        uint8_t payload[20];
        media_desc_encode(payload, &actual);
        iris_put_le32(payload + 16, total_size);
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_MEDIA_OPENED,
                               ESP_IRIS_FLAG_RESPONSE |
                                   ESP_IRIS_FLAG_STREAM_BEGIN,
                               header->request_id, state->capture.stream_id,
                               payload, sizeof(payload));
        return true;
    }
    if (header->type == ESP_IRIS_MEDIA_READ &&
        header->channel == ESP_IRIS_CHANNEL_SCREEN) {
        if (!state->capture.active || header->payload_size != 8) {
            (void)iris_queue_error(runtime, header->request_id,
                                   ESP_ERR_INVALID_STATE,
                                   header->channel, header->type);
            return true;
        }
        const uint32_t offset = iris_get_le32(frame->payload);
        uint16_t maximum = iris_get_le16(frame->payload + 4);
        if (maximum > CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES) {
            maximum = CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES;
        }
        if (maximum == 0 || offset >= state->capture.total_size) {
            (void)iris_queue_error(runtime, header->request_id,
                                   ESP_ERR_INVALID_ARG,
                                   header->channel, header->type);
            return true;
        }
        size_t size = 0;
        esp_err_t err = state->screen.read(
            offset, runtime->rx_wire + 8, maximum, &size,
            state->screen.user_ctx);
        if (err != ESP_OK || size == 0 || size > maximum ||
            offset + size > state->capture.total_size) {
            (void)iris_queue_error(runtime, header->request_id,
                                   err == ESP_OK ? ESP_ERR_INVALID_SIZE : err,
                                   header->channel, header->type);
            return true;
        }
        iris_put_le32(runtime->rx_wire, offset);
        iris_put_le32(runtime->rx_wire + 4, state->capture.total_size);
        const bool finished = offset + size == state->capture.total_size;
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_MEDIA_DATA,
                               ESP_IRIS_FLAG_RESPONSE |
                                   (finished ? ESP_IRIS_FLAG_STREAM_END : 0),
                               header->request_id, state->capture.stream_id,
                               runtime->rx_wire, size + 8);
        return true;
    }
    if (header->type == ESP_IRIS_MEDIA_CLOSE &&
        header->channel == ESP_IRIS_CHANNEL_SCREEN) {
        if (state->capture.active && state->screen.end != NULL) {
            state->screen.end(state->screen.user_ctx);
        }
        state->capture.active = false;
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_MEDIA_CLOSE,
                               ESP_IRIS_FLAG_RESPONSE |
                                   ESP_IRIS_FLAG_STREAM_END,
                               header->request_id, header->stream_id,
                               NULL, 0);
        return true;
    }
    if (header->type == ESP_IRIS_MEDIA_MIRROR_START) {
        iris_media_slot_t *slot = &state->media[index];
        const uint16_t fps = iris_get_le16(frame->payload + 16);
        const uint16_t reserved = iris_get_le16(frame->payload + 18);
        if (slot->active || fps == 0 || fps > 60 || reserved != 0 ||
            (index == 0 && state->capture.active)) {
            (void)iris_queue_error(runtime, header->request_id,
                slot->active || (index == 0 && state->capture.active)
                    ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG,
                header->channel, header->type);
            return true;
        }
        esp_err_t err = ensure_media_buffer(state, slot);
        if (err != ESP_OK) {
            (void)iris_queue_error(runtime, header->request_id, err,
                                   header->channel, header->type);
            return true;
        }
        esp_iris_media_desc_t requested;
        media_desc_decode(frame->payload, &requested);
        slot->description = requested;
        slot->frame_description = requested;
        slot->pull = false;
        slot->fps = fps;
        if (index == 0 && state->screen.begin != NULL) {
            uint32_t total_size = 0;
            esp_iris_media_desc_t actual = {0};
            err = state->screen.begin(&requested, &actual, &total_size,
                                      state->screen.user_ctx);
            const bool raw = err == ESP_OK &&
                (actual.format == ESP_IRIS_PIXEL_FORMAT_RGB565 ||
                 actual.format == ESP_IRIS_PIXEL_FORMAT_RGB888);
            if (!raw || actual.stride == 0 || actual.height == 0 ||
                actual.stride > CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES ||
                (uint64_t)total_size !=
                    (uint64_t)actual.stride * actual.height) {
                if (err == ESP_OK && state->screen.end != NULL) {
                    state->screen.end(state->screen.user_ctx);
                }
                release_media_buffer(state, slot);
                (void)iris_queue_error(runtime, header->request_id,
                    err != ESP_OK ? err : ESP_ERR_NOT_SUPPORTED,
                    header->channel, header->type);
                return true;
            }
            slot->description = actual;
            slot->frame_description = actual;
            slot->total_size = total_size;
            slot->offset = 0;
            slot->pull_frame_id = 1;
            slot->next_frame_us = 0;
            slot->pull = true;
        }
        slot->active = true;
        slot->stream_id = header->stream_id != 0
            ? header->stream_id : header->request_id;
        slot->pending = false;
        slot->dropped = 0;
        slot->credit = 0;
        uint8_t response[20];
        media_desc_encode(response, &slot->frame_description);
        iris_put_le16(response + 16, fps);
        iris_put_le16(response + 18, 0);
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_MEDIA_MIRROR_STATE,
                               ESP_IRIS_FLAG_RESPONSE |
                                   ESP_IRIS_FLAG_STREAM_BEGIN,
                               header->request_id,
                               slot->stream_id,
                               response, sizeof(response));
        return true;
    }
    if (header->type == ESP_IRIS_MEDIA_MIRROR_STOP) {
        iris_media_slot_t *slot = &state->media[index];
        if (index == 0 && slot->pull && state->screen.end != NULL) {
            state->screen.end(state->screen.user_ctx);
        }
        release_media_buffer(state, slot);
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_MEDIA_MIRROR_STATE,
                               ESP_IRIS_FLAG_RESPONSE |
                                   ESP_IRIS_FLAG_STREAM_END,
                               header->request_id, header->stream_id,
                               NULL, 0);
        maybe_release_state(state);
        return true;
    }
    (void)iris_queue_error(runtime, header->request_id,
                           ESP_ERR_NOT_SUPPORTED,
                           header->channel, header->type);
    return true;
}

#if CONFIG_ESP_IRIS_OTA
static void ota_reset_without_finish(iris_ota_t *ota)
{
    if (ota->hash_active) {
        (void)psa_hash_abort(&ota->hash);
    }
    memset(ota, 0, sizeof(*ota));
}

static void ota_job_cancel(void *user_ctx)
{
    ota_abort(user_ctx, ESP_ERR_INVALID_STATE);
}

static const esp_partition_t *ota_partition_at(uint32_t address)
{
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (partition != NULL && partition->address == address) {
            esp_partition_iterator_release(iterator);
            return partition;
        }
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);
    return NULL;
}

static const esp_partition_t *ota_select_target(void)
{
    const esp_partition_t *candidate = esp_ota_get_next_update_partition(NULL);
    if (candidate == NULL) {
        return NULL;
    }
    uint32_t address = candidate->address;
    if (esp_iris_platform_select_ota_target(candidate->address, &address) !=
        ESP_OK) {
        return NULL;
    }
    return ota_partition_at(address);
}

static bool handle_ota(iris_runtime_t *runtime,
                       const iris_decoded_frame_t *frame)
{
    const esp_iris_wire_header_t *header = &frame->header;
    if (header->channel != ESP_IRIS_CHANNEL_OTA) {
        return false;
    }
    if (header->type == ESP_IRIS_OTA_BEGIN && ota_select_target() == NULL) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_NOT_FOUND,
                               header->channel, header->type);
        return true;
    }
    const bool create = header->type == ESP_IRIS_OTA_BEGIN;
    iris_service_state_t *state = service_state(create);
    if (state == NULL) {
        (void)iris_queue_error(runtime, header->request_id,
                               create ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE,
                               header->channel, header->type);
        return true;
    }
    iris_ota_t *ota = &state->ota;
    if (header->type == ESP_IRIS_OTA_STATUS) {
        if (header->payload_size != 0) {
            (void)iris_queue_error(runtime, header->request_id,
                                   ESP_ERR_INVALID_SIZE,
                                   header->channel, header->type);
            return true;
        }
        uint8_t payload[25] = {0};
        esp_iris_job_info_t info = {0};
        if (ota->job != NULL) {
            (void)esp_iris_job_get_info(ota->job, &info);
        }
        iris_put_le32(payload, info.id);
        iris_put_le32(payload + 4, ota->total_size);
        iris_put_le32(payload + 8, ota->received);
        iris_put_le16(payload + 12, info.progress_permille);
        payload[14] = ota->active ? 1 : 0;
        const size_t label_size = ota->partition != NULL
            ? strnlen(ota->partition->label, 5) : 0;
        payload[15] = (uint8_t)label_size;
        iris_put_le32(payload + 16, (uint32_t)info.result);
        if (label_size > 0) {
            memcpy(payload + 20, ota->partition->label, label_size);
        }
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_OTA_STATUS,
                               ESP_IRIS_FLAG_RESPONSE,
                               header->request_id, info.id,
                               payload, 20 + label_size);
        return true;
    }
    if (header->type == ESP_IRIS_OTA_BEGIN) {
        if (ota->active || header->payload_size < IRIS_OTA_BEGIN_FIXED_SIZE) {
            (void)iris_queue_error(runtime, header->request_id,
                ota->active ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_SIZE,
                header->channel, header->type);
            return true;
        }
        const uint32_t total_size = iris_get_le32(frame->payload);
        const uint8_t project_length = frame->payload[36];
        const uint8_t version_length = frame->payload[37];
        if (project_length >= sizeof(ota->project_name) ||
            version_length >= sizeof(ota->version) ||
            header->payload_size != IRIS_OTA_BEGIN_FIXED_SIZE +
                                    project_length + version_length) {
            (void)iris_queue_error(runtime, header->request_id,
                                   ESP_ERR_INVALID_SIZE,
                                   header->channel, header->type);
            return true;
        }
        const esp_partition_t *partition = ota_select_target();
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (partition == NULL || running == NULL || partition == running ||
            partition->type != ESP_PARTITION_TYPE_APP ||
            total_size == 0 || total_size > partition->size) {
            (void)iris_queue_error(runtime, header->request_id,
                                   ESP_ERR_NOT_FOUND,
                                   header->channel, header->type);
            return true;
        }
        memset(ota, 0, sizeof(*ota));
        ota->partition = partition;
        ota->total_size = total_size;
        memcpy(ota->expected_sha256, frame->payload + 4, 32);
        memcpy(ota->project_name, frame->payload + 40, project_length);
        memcpy(ota->version, frame->payload + 40 + project_length,
               version_length);
        esp_err_t err = esp_iris_job_create(0x100, ota_job_cancel, state,
                                            &ota->job);
        if (err == ESP_OK) {
            ota->hash = psa_hash_operation_init();
            err = psa_crypto_init() == PSA_SUCCESS &&
                  psa_hash_setup(&ota->hash, PSA_ALG_SHA_256) == PSA_SUCCESS
                ? ESP_OK : ESP_FAIL;
            ota->hash_active = err == ESP_OK;
        }
        if (err == ESP_OK) {
            err = esp_ota_begin(partition, total_size, &ota->handle);
        }
        if (err != ESP_OK) {
            if (ota->job != NULL) {
                (void)esp_iris_job_finish(ota->job, err);
            }
            ota_reset_without_finish(ota);
            (void)iris_queue_error(runtime, header->request_id, err,
                                   header->channel, header->type);
            return true;
        }
        ota->active = true;
        uint8_t payload[16] = {0};
        esp_iris_job_info_t info;
        (void)esp_iris_job_get_info(ota->job, &info);
        iris_put_le32(payload, info.id);
        iris_put_le32(payload + 4, total_size);
        iris_put_le16(payload + 8, CONFIG_ESP_IRIS_OTA_CHUNK_BYTES);
        const size_t label_size = strnlen(partition->label, 5);
        payload[10] = (uint8_t)label_size;
        memcpy(payload + 11, partition->label, label_size);
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_OTA_BEGIN_RESPONSE,
                               ESP_IRIS_FLAG_RESPONSE |
                                   ESP_IRIS_FLAG_STREAM_BEGIN,
                               header->request_id, info.id,
                               payload, 11 + label_size);
        return true;
    }
    if (!ota->active) {
        (void)iris_queue_error(runtime, header->request_id,
                               ESP_ERR_INVALID_STATE,
                               header->channel, header->type);
        return true;
    }
    if (header->type == ESP_IRIS_OTA_CANCEL) {
        if (ota->job != NULL) {
            taskENTER_CRITICAL(&s_services_lock);
            ota->job->info.cancel_requested = true;
            taskEXIT_CRITICAL(&s_services_lock);
        }
        ota_abort(state, ESP_ERR_INVALID_STATE);
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_OTA_STATUS,
                               ESP_IRIS_FLAG_RESPONSE |
                                   ESP_IRIS_FLAG_STREAM_END,
                               header->request_id, header->stream_id,
                               NULL, 0);
        return true;
    }
    if (header->type == ESP_IRIS_OTA_DATA) {
        if (header->payload_size <= 4 ||
            header->payload_size - 4 > CONFIG_ESP_IRIS_OTA_CHUNK_BYTES ||
            iris_get_le32(frame->payload) != ota->received ||
            ota->received + header->payload_size - 4 > ota->total_size) {
            (void)iris_queue_error(runtime, header->request_id,
                                   ESP_ERR_INVALID_SIZE,
                                   header->channel, header->type);
            return true;
        }
        const uint8_t *data = frame->payload + 4;
        const size_t size = header->payload_size - 4;
        esp_err_t err = psa_hash_update(&ota->hash, data, size) == PSA_SUCCESS
            ? esp_ota_write(ota->handle, data, size) : ESP_FAIL;
        if (err != ESP_OK) {
            ota_abort(state, err);
            (void)iris_queue_error(runtime, header->request_id, err,
                                   header->channel, header->type);
            return true;
        }
        ota->received += size;
        uint16_t progress = (uint16_t)(
            ((uint64_t)ota->received * 900U) / ota->total_size);
        (void)esp_iris_job_update(ota->job, progress);
        uint8_t payload[8];
        iris_put_le32(payload, ota->received);
        iris_put_le16(payload + 4, progress);
        iris_put_le16(payload + 6, 0);
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_OTA_DATA_RESPONSE,
                               ESP_IRIS_FLAG_RESPONSE, header->request_id,
                               header->stream_id, payload, sizeof(payload));
        return true;
    }
    if (header->type == ESP_IRIS_OTA_END) {
        esp_err_t err = ota->received == ota->total_size
            ? ESP_OK : ESP_ERR_INVALID_SIZE;
        uint8_t digest[32];
        size_t digest_size = 0;
        if (err == ESP_OK &&
            psa_hash_finish(&ota->hash, digest, sizeof(digest),
                            &digest_size) != PSA_SUCCESS) {
            err = ESP_FAIL;
        }
        ota->hash_active = false;
        if (err == ESP_OK &&
            (digest_size != sizeof(digest) ||
             !constant_time_equal(digest, ota->expected_sha256,
                                  sizeof(digest)))) {
            err = ESP_ERR_INVALID_CRC;
        }
        if (err == ESP_OK) {
            err = esp_ota_end(ota->handle);
        } else {
            (void)esp_ota_abort(ota->handle);
        }
        esp_app_desc_t description;
        if (err == ESP_OK) {
            err = esp_ota_get_partition_description(ota->partition,
                                                    &description);
        }
        if (err == ESP_OK && ota->project_name[0] != '\0' &&
            strncmp(description.project_name, ota->project_name,
                    sizeof(description.project_name)) != 0) {
            err = ESP_ERR_INVALID_VERSION;
        }
        if (err == ESP_OK && ota->version[0] != '\0' &&
            strncmp(description.version, ota->version,
                    sizeof(description.version)) != 0) {
            err = ESP_ERR_INVALID_VERSION;
        }
#if CONFIG_ESP_IRIS_OTA_REQUIRE_PROJECT_NAME_MATCH
        const esp_app_desc_t *running_description = esp_app_get_description();
        if (err == ESP_OK &&
            strncmp(description.project_name,
                    running_description->project_name,
                    sizeof(description.project_name)) != 0) {
            err = ESP_ERR_INVALID_VERSION;
        }
#endif
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (err == ESP_OK) {
            err = esp_iris_platform_prepare_ota(
                running != NULL ? running->address : 0,
                ota->partition->address);
        }
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(ota->partition);
        }
        esp_iris_job_handle_t job = ota->job;
        uint32_t job_id = 0;
        if (job != NULL) {
            esp_iris_job_info_t info;
            (void)esp_iris_job_finish(job, err);
            (void)esp_iris_job_get_info(job, &info);
            job_id = info.id;
        }
        uint8_t payload[8];
        iris_put_le32(payload, job_id);
        iris_put_le32(payload + 4, (uint32_t)err);
        ota_reset_without_finish(ota);
        (void)iris_queue_frame(runtime, header->channel,
                               ESP_IRIS_OTA_END_RESPONSE,
                               ESP_IRIS_FLAG_RESPONSE |
                                   ESP_IRIS_FLAG_STREAM_END |
                                   (err == ESP_OK ? 0 : ESP_IRIS_FLAG_ERROR),
                               header->request_id, job_id,
                               payload, sizeof(payload));
        return true;
    }
    (void)iris_queue_error(runtime, header->request_id,
                           ESP_ERR_NOT_SUPPORTED,
                           header->channel, header->type);
    return true;
}
#else
static bool handle_ota(iris_runtime_t *runtime,
                       const iris_decoded_frame_t *frame)
{
    if (frame->header.channel != ESP_IRIS_CHANNEL_OTA) {
        return false;
    }
    (void)iris_queue_error(runtime, frame->header.request_id,
                           ESP_ERR_NOT_SUPPORTED, frame->header.channel,
                           frame->header.type);
    return true;
}
#endif

bool iris_services_handle_frame(iris_runtime_t *runtime,
                                const iris_decoded_frame_t *frame,
                                uint64_t received_us)
{
    if (frame->header.channel == ESP_IRIS_CHANNEL_CONTROL) {
        return handle_rpc(runtime, frame, received_us) ||
               handle_job(runtime, frame) ||
               handle_restart(runtime, frame);
    }
    return iris_files_handle_frame(runtime, frame) ||
           iris_system_inventory_handle_frame(runtime, frame) ||
           iris_system_update_handle_frame(runtime, frame) ||
           handle_media(runtime, frame) || handle_ota(runtime, frame);
}

static bool queue_job_event(iris_runtime_t *runtime,
                            iris_service_state_t *state)
{
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_JOBS; ++i) {
        struct esp_iris_job *job = &state->jobs[i];
        if (job->magic == IRIS_JOB_MAGIC && job->event_pending) {
            uint8_t payload[16];
            taskENTER_CRITICAL(&s_services_lock);
            job_encode(payload, &job->info);
            job->event_pending = false;
            taskEXIT_CRITICAL(&s_services_lock);
            return iris_queue_frame(runtime, ESP_IRIS_CHANNEL_EVENT,
                                    ESP_IRIS_EVENT_JOB_UPDATE,
                                    ESP_IRIS_FLAG_RELIABLE, 0, job->info.id,
                                    payload, sizeof(payload)) == ESP_OK;
        }
    }
    return false;
}

static bool prepare_pull_screen(iris_service_state_t *state,
                                iris_media_slot_t *slot)
{
    if (!slot->active || !slot->pull || slot->pending || slot->data == NULL ||
        state->screen.read == NULL || slot->frame_description.stride == 0) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    if (slot->offset == 0 && now < slot->next_frame_us) {
        return false;
    }
    const uint32_t stride = slot->frame_description.stride;
    const uint32_t remaining = slot->total_size - slot->offset;
    size_t size = CONFIG_ESP_IRIS_MEDIA_LATEST_BYTES / stride * stride;
    if (size > remaining) {
        size = remaining;
    }
    if (size == 0 || slot->credit < IRIS_MEDIA_DATA_HEADER_SIZE + size) {
        return false;
    }
    size_t read_size = 0;
    esp_err_t err = state->screen.read(slot->offset, slot->data, size,
                                      &read_size, state->screen.user_ctx);
    if (err != ESP_OK || read_size == 0 || read_size > size ||
        read_size % stride != 0) {
        return false;
    }
    esp_iris_media_desc_t tile = slot->frame_description;
    tile.y += slot->offset / stride;
    tile.height = read_size / stride;
    slot->description = tile;
    slot->frame_id = slot->pull_frame_id;
    slot->flags = 0;
    slot->monotonic_us = (uint64_t)now;
    slot->size = read_size;
    slot->pending = true;
    slot->offset += read_size;
    if (slot->offset == slot->total_size) {
        slot->offset = 0;
        ++slot->pull_frame_id;
        slot->next_frame_us = now + 1000000LL / slot->fps;
    }
    return true;
}

static bool queue_media(iris_runtime_t *runtime,
                        iris_service_state_t *state)
{
    for (size_t i = 0; i < 3; ++i) {
        iris_media_slot_t *slot = &state->media[i];
        if (i == 0) {
            (void)prepare_pull_screen(state, slot);
        }
        const size_t payload_size = IRIS_MEDIA_DATA_HEADER_SIZE + slot->size;
        if (!slot->active || !slot->pending || slot->data == NULL ||
            slot->credit < payload_size) {
            continue;
        }
        taskENTER_CRITICAL(&s_services_lock);
        iris_put_le64(runtime->rx_wire, slot->monotonic_us);
        iris_put_le32(runtime->rx_wire + 8, slot->frame_id);
        iris_put_le32(runtime->rx_wire + 12, slot->dropped);
        iris_put_le16(runtime->rx_wire + 16, slot->flags);
        iris_put_le16(runtime->rx_wire + 18, (uint16_t)slot->size);
        media_desc_encode(runtime->rx_wire + 20, &slot->description);
        memcpy(runtime->rx_wire + IRIS_MEDIA_DATA_HEADER_SIZE,
               slot->data, slot->size);
        slot->pending = false;
        slot->credit -= payload_size;
        taskEXIT_CRITICAL(&s_services_lock);
        return iris_queue_frame(runtime,
                                ESP_IRIS_CHANNEL_SCREEN + i,
                                ESP_IRIS_MEDIA_DATA, 0, 0,
                                slot->stream_id, runtime->rx_wire,
                                payload_size) == ESP_OK;
    }
    return false;
}

bool iris_services_queue_next(iris_runtime_t *runtime)
{
    if (iris_files_queue_next(runtime)) {
        return true;
    }
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state)) {
        return false;
    }
    return queue_job_event(runtime, state) || queue_media(runtime, state);
}

void iris_services_poll(iris_runtime_t *runtime)
{
    iris_service_state_t *state = service_state(false);
    if (!valid_state(state) || !state->restart_pending ||
        runtime->tx_wire_length != 0 ||
        esp_timer_get_time() < state->restart_at_us) {
        return;
    }
    state->restart_pending = false;
    esp_restart();
}

uint32_t iris_services_allocated_bytes(void)
{
    iris_service_state_t *state = service_state(false);
    return (valid_state(state) ? state->allocated_bytes : 0) +
        iris_files_allocated_bytes();
}

uint32_t iris_services_static_bytes(void)
{
    return sizeof(s_services) + sizeof(s_services_lock) +
        iris_files_static_bytes() + iris_system_inventory_static_bytes() +
        iris_system_update_static_bytes();
}
