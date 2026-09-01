#include "esp_iris_internal.h"

#include <string.h>

#include "esp_iris_system_update.h"
#include "psa/crypto.h"

#define IRIS_SYSTEM_BEGIN_FIXED_SIZE 56U
#define IRIS_SYSTEM_COMPONENT_BEGIN_SIZE 60U
#define IRIS_SYSTEM_DATA_HEADER_SIZE 24U
#define IRIS_SYSTEM_OPERATION_PAYLOAD_SIZE 16U
#define IRIS_SYSTEM_JOB_KIND 0x101U

#if CONFIG_ESP_IRIS_SYSTEM_UPDATE

_Static_assert(IRIS_SYSTEM_BEGIN_FIXED_SIZE +
                       CONFIG_ESP_IRIS_SYSTEM_UPDATE_MANIFEST_BYTES +
                       CONFIG_ESP_IRIS_SYSTEM_UPDATE_SIGNATURE_BYTES <=
                   ESP_IRIS_MAX_PAYLOAD_SIZE,
               "system-update manifest and signature exceed one frame");

typedef struct {
    bool backend_registered;
    esp_iris_system_update_backend_t backend;
    esp_iris_system_update_status_t status;
    esp_iris_system_update_status_t published_status;
    esp_iris_system_update_component_t component;
    uint8_t manifest_sha256[ESP_IRIS_SYSTEM_SHA256_BYTES];
    uint8_t flags;
    uint8_t completed_ids[CONFIG_ESP_IRIS_SYSTEM_UPDATE_MAX_COMPONENTS];
    psa_hash_operation_t hash;
    bool hash_active;
    esp_iris_job_handle_t job;
} iris_system_update_state_t;

static iris_system_update_state_t s_system_update;
static portMUX_TYPE s_system_update_lock = portMUX_INITIALIZER_UNLOCKED;

static void publish_status(void)
{
    taskENTER_CRITICAL(&s_system_update_lock);
    s_system_update.published_status = s_system_update.status;
    taskEXIT_CRITICAL(&s_system_update_lock);
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right, size_t size)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

static bool operation_matches(const uint8_t *operation_id)
{
    return bytes_equal(operation_id, s_system_update.status.operation_id,
                       ESP_IRIS_SYSTEM_OPERATION_ID_BYTES);
}

static bool operation_id_valid(const uint8_t *operation_id)
{
    uint8_t combined = 0;
    for (size_t i = 0; i < ESP_IRIS_SYSTEM_OPERATION_ID_BYTES; ++i) {
        combined |= operation_id[i];
    }
    return combined != 0;
}

static bool update_in_progress(void)
{
    return s_system_update.status.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_PREPARED ||
           s_system_update.status.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING ||
           s_system_update.status.phase ==
               ESP_IRIS_SYSTEM_UPDATE_PHASE_COMPONENT_VERIFIED ||
           s_system_update.status.phase == ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTING;
}

static void hash_abort(void)
{
    if (s_system_update.hash_active) {
        (void)psa_hash_abort(&s_system_update.hash);
        s_system_update.hash_active = false;
    }
}

static void backend_abort(esp_err_t reason,
                          esp_iris_system_update_phase_t phase)
{
    if (!update_in_progress()) {
        return;
    }
    hash_abort();
    if (s_system_update.backend.abort != NULL) {
        s_system_update.backend.abort(s_system_update.status.operation_id,
                                      reason,
                                      s_system_update.backend.user_ctx);
    }
    s_system_update.status.phase = phase;
    s_system_update.status.result = reason;
    s_system_update.status.active_component_id = 0;
    s_system_update.status.component_received = 0;
    s_system_update.status.component_size = 0;
    memset(&s_system_update.component, 0,
           sizeof(s_system_update.component));
    publish_status();
}

static void job_cancel(void *user_ctx)
{
    (void)user_ctx;
    backend_abort(ESP_ERR_INVALID_STATE,
                  ESP_IRIS_SYSTEM_UPDATE_PHASE_CANCELLED);
    if (s_system_update.job != NULL) {
        (void)esp_iris_job_finish(s_system_update.job,
                                  ESP_ERR_INVALID_STATE);
    }
}

static uint32_t job_id(void)
{
    esp_iris_job_info_t info = {0};
    if (s_system_update.job != NULL) {
        (void)esp_iris_job_get_info(s_system_update.job, &info);
    }
    return info.id;
}

static uint16_t transfer_progress(void)
{
    const uint32_t count = s_system_update.status.component_count;
    if (count == 0) {
        return 0;
    }
    uint64_t units = (uint64_t)s_system_update.status.completed_components *
                     850U;
    if (s_system_update.status.component_size > 0) {
        units += (uint64_t)s_system_update.status.component_received * 850U /
                 s_system_update.status.component_size;
    }
    uint32_t progress = 50U + (uint32_t)(units / count);
    return (uint16_t)(progress > 900U ? 900U : progress);
}

static bool completed_id(uint8_t id)
{
    for (uint8_t i = 0;
         i < s_system_update.status.completed_components; ++i) {
        if (s_system_update.completed_ids[i] == id) {
            return true;
        }
    }
    return false;
}

static esp_err_t manifest_digest(const uint8_t *manifest, size_t size,
                                 uint8_t digest[32])
{
    size_t digest_size = 0;
    const psa_status_t status = psa_crypto_init() == PSA_SUCCESS
        ? psa_hash_compute(PSA_ALG_SHA_256, manifest, size, digest, 32,
                           &digest_size)
        : PSA_ERROR_GENERIC_ERROR;
    return status == PSA_SUCCESS && digest_size == 32 ? ESP_OK : ESP_FAIL;
}

static void encode_status(uint8_t payload[36])
{
    esp_iris_system_update_status_t status;
    taskENTER_CRITICAL(&s_system_update_lock);
    status = s_system_update.published_status;
    taskEXIT_CRITICAL(&s_system_update_lock);
    memcpy(payload, status.operation_id, 16);
    iris_put_le32(payload + 16, job_id());
    payload[20] = (uint8_t)status.phase;
    payload[21] = status.component_count;
    payload[22] = status.completed_components;
    payload[23] = status.active_component_id;
    iris_put_le32(payload + 24, status.component_received);
    iris_put_le32(payload + 28, status.component_size);
    iris_put_le32(payload + 32, (uint32_t)status.result);
}

static bool handle_status(iris_runtime_t *runtime,
                          const iris_decoded_frame_t *frame)
{
    if (!s_system_update.backend_registered) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_NOT_SUPPORTED, frame->header.channel,
                               frame->header.type);
        return true;
    }
    if (frame->header.payload_size != 0) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_SIZE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    uint8_t payload[36];
    encode_status(payload);
    (void)iris_queue_frame(runtime, frame->header.channel,
                           ESP_IRIS_SYSTEM_UPDATE_STATUS_RESPONSE,
                           ESP_IRIS_FLAG_RESPONSE, frame->header.request_id,
                           job_id(), payload, sizeof(payload));
    return true;
}

static void queue_begin_response(iris_runtime_t *runtime,
                                 const iris_decoded_frame_t *frame)
{
    uint8_t response[24];
    memcpy(response, s_system_update.status.operation_id, 16);
    iris_put_le32(response + 16, job_id());
    iris_put_le16(response + 20, CONFIG_ESP_IRIS_SYSTEM_UPDATE_CHUNK_BYTES);
    response[22] = s_system_update.status.component_count;
    response[23] = s_system_update.flags;
    (void)iris_queue_frame(runtime, frame->header.channel,
                           ESP_IRIS_SYSTEM_UPDATE_BEGIN_RESPONSE,
                           ESP_IRIS_FLAG_RESPONSE |
                               ESP_IRIS_FLAG_STREAM_BEGIN,
                           frame->header.request_id, job_id(), response,
                           sizeof(response));
}

static bool handle_begin(iris_runtime_t *runtime,
                         const iris_decoded_frame_t *frame)
{
    const uint8_t *payload = frame->payload;
    const size_t payload_size = frame->header.payload_size;
    if (!s_system_update.backend_registered) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_NOT_SUPPORTED,
                               frame->header.channel, frame->header.type);
        return true;
    }
    if (payload_size < IRIS_SYSTEM_BEGIN_FIXED_SIZE) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_SIZE,
                               frame->header.channel, frame->header.type);
        return true;
    }
    const uint16_t manifest_size = iris_get_le16(payload + 16);
    const uint16_t signature_size = iris_get_le16(payload + 18);
    const uint8_t component_count = payload[20];
    const uint8_t flags = payload[21];
    if (!operation_id_valid(payload) || iris_get_le16(payload + 22) != 0 ||
        manifest_size == 0 ||
        manifest_size > CONFIG_ESP_IRIS_SYSTEM_UPDATE_MANIFEST_BYTES ||
        signature_size > CONFIG_ESP_IRIS_SYSTEM_UPDATE_SIGNATURE_BYTES ||
        component_count == 0 ||
        component_count > CONFIG_ESP_IRIS_SYSTEM_UPDATE_MAX_COMPONENTS ||
        payload_size != IRIS_SYSTEM_BEGIN_FIXED_SIZE + manifest_size +
                            signature_size) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_SIZE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    uint8_t actual_manifest_sha[32] = {0};
    const uint8_t *manifest_bytes = payload + IRIS_SYSTEM_BEGIN_FIXED_SIZE;
    esp_err_t err = manifest_digest(manifest_bytes, manifest_size,
                                    actual_manifest_sha);
    if (err == ESP_OK &&
        !bytes_equal(actual_manifest_sha, payload + 24, 32)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) {
        (void)iris_queue_error(runtime, frame->header.request_id, err,
                               frame->header.channel, frame->header.type);
        return true;
    }
    if (update_in_progress()) {
        const bool duplicate = operation_matches(payload) &&
            s_system_update.status.phase ==
                ESP_IRIS_SYSTEM_UPDATE_PHASE_PREPARED &&
            s_system_update.status.completed_components == 0 &&
            s_system_update.status.active_component_id == 0 &&
            s_system_update.status.component_count == component_count &&
            s_system_update.flags == flags &&
            bytes_equal(actual_manifest_sha,
                        s_system_update.manifest_sha256, 32);
        if (duplicate) {
            queue_begin_response(runtime, frame);
            return true;
        }
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_STATE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    esp_iris_system_update_manifest_t manifest = {
        .manifest = manifest_bytes,
        .manifest_size = manifest_size,
        .signature = manifest_bytes + manifest_size,
        .signature_size = signature_size,
        .component_count = component_count,
        .flags = flags,
    };
    memcpy(manifest.operation_id, payload, 16);
    memcpy(manifest.manifest_sha256, actual_manifest_sha, 32);
    err = s_system_update.backend.prepare(
        &manifest, s_system_update.backend.user_ctx);
    if (err != ESP_OK) {
        s_system_update.backend.abort(manifest.operation_id, err,
                                      s_system_update.backend.user_ctx);
        (void)iris_queue_error(runtime, frame->header.request_id, err,
                               frame->header.channel, frame->header.type);
        return true;
    }
    memset(&s_system_update.status, 0, sizeof(s_system_update.status));
    memset(s_system_update.completed_ids, 0,
           sizeof(s_system_update.completed_ids));
    memcpy(s_system_update.status.operation_id, payload, 16);
    s_system_update.status.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_PREPARED;
    s_system_update.status.component_count = component_count;
    s_system_update.status.result = ESP_OK;
    memcpy(s_system_update.manifest_sha256, actual_manifest_sha, 32);
    s_system_update.flags = flags;
    publish_status();
    s_system_update.job = NULL;
    err = esp_iris_job_create(IRIS_SYSTEM_JOB_KIND, job_cancel, NULL,
                              &s_system_update.job);
    if (err != ESP_OK) {
        backend_abort(err, ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED);
        (void)iris_queue_error(runtime, frame->header.request_id, err,
                               frame->header.channel, frame->header.type);
        return true;
    }
    (void)esp_iris_job_update(s_system_update.job, 50);
    queue_begin_response(runtime, frame);
    return true;
}

static bool handle_component_begin(iris_runtime_t *runtime,
                                   const iris_decoded_frame_t *frame)
{
    const uint8_t *payload = frame->payload;
    if (frame->header.payload_size != IRIS_SYSTEM_COMPONENT_BEGIN_SIZE ||
        !operation_matches(payload) ||
        s_system_update.status.phase !=
            ESP_IRIS_SYSTEM_UPDATE_PHASE_PREPARED ||
        s_system_update.status.active_component_id != 0) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_STATE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    esp_iris_system_update_component_t component = {
        .id = payload[16],
        .kind = (esp_iris_system_update_component_kind_t)payload[17],
        .flags = iris_get_le16(payload + 18),
        .target_offset = iris_get_le32(payload + 20),
        .size = iris_get_le32(payload + 24),
    };
    memcpy(component.sha256, payload + 28, 32);
    if (component.id == 0 || component.size == 0 ||
        component.kind < ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BOOTLOADER ||
        component.kind > ESP_IRIS_SYSTEM_UPDATE_COMPONENT_DATA ||
        completed_id(component.id)) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_ARG, frame->header.channel,
                               frame->header.type);
        return true;
    }
    esp_err_t err = s_system_update.backend.begin_component(
        &component, s_system_update.backend.user_ctx);
    if (err == ESP_OK) {
        s_system_update.hash = psa_hash_operation_init();
        err = psa_crypto_init() == PSA_SUCCESS &&
                      psa_hash_setup(&s_system_update.hash,
                                     PSA_ALG_SHA_256) == PSA_SUCCESS
                  ? ESP_OK
                  : ESP_FAIL;
        s_system_update.hash_active = err == ESP_OK;
    }
    if (err != ESP_OK) {
        backend_abort(err, ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED);
        (void)esp_iris_job_finish(s_system_update.job, err);
        (void)iris_queue_error(runtime, frame->header.request_id, err,
                               frame->header.channel, frame->header.type);
        return true;
    }
    s_system_update.component = component;
    s_system_update.status.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING;
    s_system_update.status.active_component_id = component.id;
    s_system_update.status.component_received = 0;
    s_system_update.status.component_size = component.size;
    publish_status();
    uint8_t response[24];
    memcpy(response, s_system_update.status.operation_id, 16);
    response[16] = component.id;
    response[17] = (uint8_t)component.kind;
    iris_put_le16(response + 18, CONFIG_ESP_IRIS_SYSTEM_UPDATE_CHUNK_BYTES);
    iris_put_le32(response + 20, component.size);
    (void)iris_queue_frame(runtime, frame->header.channel,
                           ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BEGIN_RESPONSE,
                           ESP_IRIS_FLAG_RESPONSE,
                           frame->header.request_id, job_id(), response,
                           sizeof(response));
    return true;
}

static bool handle_data(iris_runtime_t *runtime,
                        const iris_decoded_frame_t *frame)
{
    const uint8_t *payload = frame->payload;
    if (frame->header.payload_size <= IRIS_SYSTEM_DATA_HEADER_SIZE ||
        frame->header.payload_size - IRIS_SYSTEM_DATA_HEADER_SIZE >
            CONFIG_ESP_IRIS_SYSTEM_UPDATE_CHUNK_BYTES ||
        s_system_update.status.phase !=
            ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING ||
        !operation_matches(payload) ||
        payload[16] != s_system_update.component.id || payload[17] != 0 ||
        iris_get_le16(payload + 18) != 0 ||
        iris_get_le32(payload + 20) !=
            s_system_update.status.component_received) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_SIZE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    const uint8_t *data = payload + IRIS_SYSTEM_DATA_HEADER_SIZE;
    const size_t size = frame->header.payload_size -
                        IRIS_SYSTEM_DATA_HEADER_SIZE;
    if (size > s_system_update.component.size -
                   s_system_update.status.component_received) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_SIZE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    const uint32_t offset = s_system_update.status.component_received;
    esp_err_t err = psa_hash_update(&s_system_update.hash, data, size) ==
                            PSA_SUCCESS
        ? s_system_update.backend.write_component(
              &s_system_update.component, offset, data, size,
              s_system_update.backend.user_ctx)
        : ESP_FAIL;
    if (err != ESP_OK) {
        backend_abort(err, ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED);
        (void)esp_iris_job_finish(s_system_update.job, err);
        (void)iris_queue_error(runtime, frame->header.request_id, err,
                               frame->header.channel, frame->header.type);
        return true;
    }
    s_system_update.status.component_received += (uint32_t)size;
    publish_status();
    const uint16_t progress = transfer_progress();
    (void)esp_iris_job_update(s_system_update.job, progress);
    uint8_t response[24] = {0};
    memcpy(response, s_system_update.status.operation_id, 16);
    response[16] = s_system_update.component.id;
    iris_put_le16(response + 18, progress);
    iris_put_le32(response + 20,
                  s_system_update.status.component_received);
    (void)iris_queue_frame(runtime, frame->header.channel,
                           ESP_IRIS_SYSTEM_UPDATE_DATA_RESPONSE,
                           ESP_IRIS_FLAG_RESPONSE,
                           frame->header.request_id, job_id(), response,
                           sizeof(response));
    return true;
}

static bool handle_component_end(iris_runtime_t *runtime,
                                 const iris_decoded_frame_t *frame)
{
    const uint8_t *payload = frame->payload;
    if (frame->header.payload_size != 20 ||
        !operation_matches(payload) || payload[16] != s_system_update.component.id ||
        payload[17] != 0 || iris_get_le16(payload + 18) != 0 ||
        s_system_update.status.phase !=
            ESP_IRIS_SYSTEM_UPDATE_PHASE_RECEIVING ||
        s_system_update.status.component_received !=
            s_system_update.component.size) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_STATE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    uint8_t digest[32];
    size_t digest_size = 0;
    esp_err_t err = psa_hash_finish(&s_system_update.hash, digest,
                                    sizeof(digest), &digest_size) ==
                            PSA_SUCCESS &&
                        digest_size == sizeof(digest)
        ? ESP_OK
        : ESP_FAIL;
    s_system_update.hash_active = false;
    if (err == ESP_OK &&
        !bytes_equal(digest, s_system_update.component.sha256, 32)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) {
        err = s_system_update.backend.end_component(
            &s_system_update.component, digest,
            s_system_update.backend.user_ctx);
    }
    if (err != ESP_OK) {
        backend_abort(err, ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED);
        (void)esp_iris_job_finish(s_system_update.job, err);
        (void)iris_queue_error(runtime, frame->header.request_id, err,
                               frame->header.channel, frame->header.type);
        return true;
    }
    s_system_update.completed_ids[
        s_system_update.status.completed_components++] =
        s_system_update.component.id;
    s_system_update.status.phase =
        ESP_IRIS_SYSTEM_UPDATE_PHASE_COMPONENT_VERIFIED;
    s_system_update.status.active_component_id = 0;
    s_system_update.status.component_received = 0;
    s_system_update.status.component_size = 0;
    const uint8_t completed =
        s_system_update.status.completed_components;
    const uint8_t component_id = s_system_update.component.id;
    memset(&s_system_update.component, 0,
           sizeof(s_system_update.component));
    if (completed < s_system_update.status.component_count) {
        s_system_update.status.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_PREPARED;
    }
    publish_status();
    (void)esp_iris_job_update(s_system_update.job, transfer_progress());
    uint8_t response[24] = {0};
    memcpy(response, s_system_update.status.operation_id, 16);
    response[16] = component_id;
    response[17] = completed;
    iris_put_le32(response + 20, (uint32_t)ESP_OK);
    (void)iris_queue_frame(runtime, frame->header.channel,
                           ESP_IRIS_SYSTEM_UPDATE_COMPONENT_END_RESPONSE,
                           ESP_IRIS_FLAG_RESPONSE,
                           frame->header.request_id, job_id(), response,
                           sizeof(response));
    return true;
}

static bool handle_commit(iris_runtime_t *runtime,
                          const iris_decoded_frame_t *frame)
{
    if (frame->header.payload_size != IRIS_SYSTEM_OPERATION_PAYLOAD_SIZE ||
        !operation_matches(frame->payload) ||
        s_system_update.status.phase !=
            ESP_IRIS_SYSTEM_UPDATE_PHASE_COMPONENT_VERIFIED ||
        s_system_update.status.completed_components !=
            s_system_update.status.component_count) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_STATE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    s_system_update.status.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTING;
    publish_status();
    (void)esp_iris_job_update(s_system_update.job, 950);
    const esp_err_t err = s_system_update.backend.commit(
        s_system_update.status.operation_id,
        s_system_update.backend.user_ctx);
    s_system_update.status.result = err;
    s_system_update.status.phase = err == ESP_OK
        ? ESP_IRIS_SYSTEM_UPDATE_PHASE_COMMITTED
        : ESP_IRIS_SYSTEM_UPDATE_PHASE_FAILED;
    publish_status();
    if (err != ESP_OK && s_system_update.backend.abort != NULL) {
        s_system_update.backend.abort(s_system_update.status.operation_id, err,
                                      s_system_update.backend.user_ctx);
    }
    (void)esp_iris_job_finish(s_system_update.job, err);
    uint8_t response[24];
    memcpy(response, s_system_update.status.operation_id, 16);
    iris_put_le32(response + 16, job_id());
    iris_put_le32(response + 20, (uint32_t)err);
    (void)iris_queue_frame(runtime, frame->header.channel,
                           ESP_IRIS_SYSTEM_UPDATE_COMMIT_RESPONSE,
                           ESP_IRIS_FLAG_RESPONSE |
                               ESP_IRIS_FLAG_STREAM_END |
                               (err == ESP_OK ? 0 : ESP_IRIS_FLAG_ERROR),
                           frame->header.request_id, job_id(), response,
                           sizeof(response));
    return true;
}

static bool handle_cancel(iris_runtime_t *runtime,
                          const iris_decoded_frame_t *frame)
{
    if (frame->header.payload_size != IRIS_SYSTEM_OPERATION_PAYLOAD_SIZE ||
        !operation_matches(frame->payload) || !update_in_progress()) {
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_INVALID_STATE, frame->header.channel,
                               frame->header.type);
        return true;
    }
    backend_abort(ESP_ERR_INVALID_STATE,
                  ESP_IRIS_SYSTEM_UPDATE_PHASE_CANCELLED);
    (void)esp_iris_job_finish(s_system_update.job, ESP_ERR_INVALID_STATE);
    uint8_t response[36];
    encode_status(response);
    (void)iris_queue_frame(runtime, frame->header.channel,
                           ESP_IRIS_SYSTEM_UPDATE_STATUS_RESPONSE,
                           ESP_IRIS_FLAG_RESPONSE |
                               ESP_IRIS_FLAG_STREAM_END,
                           frame->header.request_id, job_id(), response,
                           sizeof(response));
    return true;
}

esp_err_t esp_iris_system_update_register(
    const esp_iris_system_update_backend_t *backend)
{
    if (backend == NULL || backend->prepare == NULL ||
        backend->begin_component == NULL ||
        backend->write_component == NULL || backend->end_component == NULL ||
        backend->commit == NULL || backend->abort == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_iris_is_started() || s_system_update.backend_registered ||
        !iris_system_inventory_registered()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_system_update.backend = *backend;
    s_system_update.backend_registered = true;
    s_system_update.status.phase = ESP_IRIS_SYSTEM_UPDATE_PHASE_IDLE;
    s_system_update.status.result = ESP_OK;
    publish_status();
    return ESP_OK;
}

esp_err_t esp_iris_system_update_unregister(void *user_ctx)
{
    if (esp_iris_is_started() || !s_system_update.backend_registered ||
        s_system_update.backend.user_ctx != user_ctx || update_in_progress()) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_system_update, 0, sizeof(s_system_update));
    return ESP_OK;
}

esp_err_t esp_iris_system_update_get_status(
    esp_iris_system_update_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_system_update_lock);
    *out_status = s_system_update.published_status;
    taskEXIT_CRITICAL(&s_system_update_lock);
    return s_system_update.backend_registered ? ESP_OK
                                               : ESP_ERR_NOT_SUPPORTED;
}

void iris_system_update_session_end(void)
{
    backend_abort(ESP_ERR_INVALID_STATE,
                  ESP_IRIS_SYSTEM_UPDATE_PHASE_CANCELLED);
    s_system_update.job = NULL;
}

bool iris_system_update_handle_frame(iris_runtime_t *runtime,
                                     const iris_decoded_frame_t *frame)
{
    if (frame->header.channel != ESP_IRIS_CHANNEL_SYSTEM_UPDATE) {
        return false;
    }
    switch (frame->header.type) {
    case ESP_IRIS_SYSTEM_UPDATE_STATUS:
        return handle_status(runtime, frame);
    case ESP_IRIS_SYSTEM_UPDATE_BEGIN:
        return handle_begin(runtime, frame);
    case ESP_IRIS_SYSTEM_UPDATE_COMPONENT_BEGIN:
        return handle_component_begin(runtime, frame);
    case ESP_IRIS_SYSTEM_UPDATE_DATA:
        return handle_data(runtime, frame);
    case ESP_IRIS_SYSTEM_UPDATE_COMPONENT_END:
        return handle_component_end(runtime, frame);
    case ESP_IRIS_SYSTEM_UPDATE_COMMIT:
        return handle_commit(runtime, frame);
    case ESP_IRIS_SYSTEM_UPDATE_CANCEL:
        return handle_cancel(runtime, frame);
    default:
        (void)iris_queue_error(runtime, frame->header.request_id,
                               ESP_ERR_NOT_SUPPORTED, frame->header.channel,
                               frame->header.type);
        return true;
    }
}

uint64_t iris_system_update_capabilities(void)
{
    return s_system_update.backend_registered ? ESP_IRIS_CAP_SYSTEM_UPDATE : 0;
}

uint32_t iris_system_update_static_bytes(void)
{
    return sizeof(s_system_update) + sizeof(s_system_update_lock);
}

bool iris_system_update_backend_registered(void)
{
    return s_system_update.backend_registered;
}

#else

esp_err_t esp_iris_system_update_register(
    const esp_iris_system_update_backend_t *backend)
{
    (void)backend;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_system_update_unregister(void *user_ctx)
{
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_system_update_get_status(
    esp_iris_system_update_status_t *out_status)
{
    (void)out_status;
    return ESP_ERR_NOT_SUPPORTED;
}

void iris_system_update_session_end(void)
{
}

bool iris_system_update_handle_frame(iris_runtime_t *runtime,
                                     const iris_decoded_frame_t *frame)
{
    if (frame->header.channel != ESP_IRIS_CHANNEL_SYSTEM_UPDATE) {
        return false;
    }
    (void)iris_queue_error(runtime, frame->header.request_id,
                           ESP_ERR_NOT_SUPPORTED, frame->header.channel,
                           frame->header.type);
    return true;
}

uint64_t iris_system_update_capabilities(void)
{
    return 0;
}

uint32_t iris_system_update_static_bytes(void)
{
    return 0;
}

bool iris_system_update_backend_registered(void)
{
    return false;
}

#endif
