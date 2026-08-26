#include "esp_iris_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_random.h"
#include "psa/crypto.h"

#define IRIS_FILE_PATH_REQUEST_HEADER_SIZE 4U
#define IRIS_FILE_VOLUME_RESPONSE_HEADER_SIZE 12U
#define IRIS_FILE_LIST_ENTRY_HEADER_SIZE 28U
#define IRIS_FILE_LIST_DATA_HEADER_SIZE 4U
#define IRIS_FILE_READ_DATA_HEADER_SIZE 20U
#define IRIS_FILE_WRITE_DATA_HEADER_SIZE 12U
#define IRIS_FILE_WRITE_OPEN_TRAILER_SIZE 20U
#define IRIS_FILE_RENAME_HEADER_SIZE 8U
#define IRIS_FILE_RENAME_PAYLOAD_SIZE \
    (IRIS_FILE_RENAME_HEADER_SIZE + ESP_IRIS_FILE_VOLUME_ID_MAX + \
     ESP_IRIS_FILE_PATH_MAX * 2U)
#define IRIS_FILE_WRITE_PAYLOAD_SIZE \
    (IRIS_FILE_WRITE_DATA_HEADER_SIZE + CONFIG_ESP_IRIS_FILE_CHUNK_BYTES)
#define IRIS_FILE_WORK_PAYLOAD_SIZE \
    (IRIS_FILE_WRITE_PAYLOAD_SIZE > IRIS_FILE_RENAME_PAYLOAD_SIZE \
         ? IRIS_FILE_WRITE_PAYLOAD_SIZE : IRIS_FILE_RENAME_PAYLOAD_SIZE)
#define IRIS_FILE_READ_PAYLOAD_SIZE \
    (IRIS_FILE_READ_DATA_HEADER_SIZE + CONFIG_ESP_IRIS_FILE_CHUNK_BYTES)
#define IRIS_FILE_LIST_PAYLOAD_SIZE \
    (IRIS_FILE_LIST_DATA_HEADER_SIZE + IRIS_FILE_LIST_ENTRY_HEADER_SIZE + \
     ESP_IRIS_FILE_PATH_MAX)
#define IRIS_FILE_COMPLETION_PAYLOAD_SIZE \
    (IRIS_FILE_READ_PAYLOAD_SIZE > IRIS_FILE_LIST_PAYLOAD_SIZE \
         ? IRIS_FILE_READ_PAYLOAD_SIZE : IRIS_FILE_LIST_PAYLOAD_SIZE)
#define IRIS_FILE_FULL_PATH_SIZE (ESP_IRIS_FILE_PATH_MAX * 2U + 2U)
#define IRIS_FILE_TEMP_PATH_SIZE (IRIS_FILE_FULL_PATH_SIZE + 24U)
#define IRIS_FILE_LIST_END 1U
#define IRIS_FILE_WRITE_ACTIVE 1U
#define IRIS_FILE_WRITE_COMMITTED 2U
#define IRIS_FILE_WRITE_ABORTED 3U

typedef struct {
    bool used;
    char id[ESP_IRIS_FILE_VOLUME_ID_MAX + 1U];
    char base_path[ESP_IRIS_FILE_PATH_MAX + 1U];
    uint32_t capabilities;
} iris_file_volume_t;

typedef enum {
    IRIS_FILE_WORK_FRAME,
    IRIS_FILE_WORK_SESSION_END,
    IRIS_FILE_WORK_STOP,
} iris_file_work_kind_t;

typedef struct {
    iris_file_work_kind_t kind;
    uint32_t token;
    uint8_t type;
    uint32_t session_id;
    uint32_t request_id;
    uint32_t stream_id;
    uint16_t payload_size;
    uint8_t payload[IRIS_FILE_WORK_PAYLOAD_SIZE];
} iris_file_work_t;

typedef struct {
    uint32_t token;
    uint8_t type;
    uint16_t flags;
    uint32_t session_id;
    uint32_t request_id;
    uint32_t stream_id;
    uint16_t payload_size;
    uint8_t payload[IRIS_FILE_COMPLETION_PAYLOAD_SIZE];
} iris_file_completion_t;

typedef enum {
    IRIS_FILE_HANDLE_NONE,
    IRIS_FILE_HANDLE_LIST,
    IRIS_FILE_HANDLE_READ,
    IRIS_FILE_HANDLE_WRITE,
} iris_file_handle_kind_t;

typedef struct {
    iris_file_handle_kind_t kind;
    uint32_t session_id;
    uint32_t stream_id;
    DIR *directory;
    int fd;
    uint64_t size;
    uint64_t committed;
    uint64_t original_etag;
    bool target_existed;
    bool overwrite;
    bool hash_active;
    psa_hash_operation_t hash;
    char directory_path[IRIS_FILE_FULL_PATH_SIZE];
    char target_path[IRIS_FILE_FULL_PATH_SIZE];
    char temp_path[IRIS_FILE_TEMP_PATH_SIZE];
} iris_file_handle_t;

typedef struct {
    bool valid;
    uint32_t session_id;
    uint32_t stream_id;
    uint64_t committed;
    uint64_t expected;
    uint8_t state;
    esp_iris_file_status_t result;
} iris_file_write_receipt_t;

static iris_file_volume_t s_volumes[CONFIG_ESP_IRIS_MAX_FILE_VOLUMES];
static portMUX_TYPE s_volume_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_work_queue;
static QueueHandle_t s_completion_queue;
static TaskHandle_t s_file_task;
static atomic_uint s_active_token = ATOMIC_VAR_INIT(0);
static uint32_t s_next_token;
static iris_file_work_t s_task_work;
static iris_file_completion_t s_task_completion;
static iris_file_handle_t s_task_handle;
static iris_file_write_receipt_t s_write_receipt;

/* ESP-IDF's VFS exposes stat(), but not lstat(). The supported backing file
 * systems (LittleFS, SPIFFS and FATFS) do not implement symbolic links. */
static int file_stat(const char *path, struct stat *metadata)
{
    return stat(path, metadata);
}

static bool valid_utf8(const uint8_t *value, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const uint8_t first = value[offset++];
        if (first < 0x80U) {
            if (first == 0 || first < 0x20U || first == 0x7fU) {
                return false;
            }
            continue;
        }
        size_t continuation = 0;
        uint32_t codepoint = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation = 1;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation = 2;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (continuation > size - offset) {
            return false;
        }
        for (size_t i = 0; i < continuation; ++i) {
            const uint8_t next = value[offset++];
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (next & 0x3fU);
        }
        if ((continuation == 2 && codepoint < 0x800U) ||
            (continuation == 3 && codepoint < 0x10000U) ||
            codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

static bool valid_volume_id(const char *id)
{
    if (id == NULL) {
        return false;
    }
    const size_t size = strnlen(id, ESP_IRIS_FILE_VOLUME_ID_MAX + 1U);
    if (size == 0 || size > ESP_IRIS_FILE_VOLUME_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        const char value = id[i];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '_' || value == '-')) {
            return false;
        }
    }
    return true;
}

static bool valid_relative_path(const uint8_t *path, size_t size)
{
    if (size == 0) {
        return true;
    }
    if (path == NULL || size > ESP_IRIS_FILE_PATH_MAX || path[0] == '/' ||
        path[size - 1U] == '/' || !valid_utf8(path, size)) {
        return false;
    }
    size_t component_start = 0;
    for (size_t i = 0; i <= size; ++i) {
        if (i < size && path[i] != '/') {
            if (path[i] == '\\') {
                return false;
            }
            continue;
        }
        const size_t component_size = i - component_start;
        if (component_size == 0 ||
            (component_size == 1 && path[component_start] == '.') ||
            (component_size == 2 && path[component_start] == '.' &&
             path[component_start + 1U] == '.')) {
            return false;
        }
        component_start = i + 1U;
    }
    return true;
}

static esp_iris_file_status_t status_from_errno(int value)
{
    switch (value) {
    case ENOENT:
        return ESP_IRIS_FILE_STATUS_NOT_FOUND;
    case ENOTDIR:
        return ESP_IRIS_FILE_STATUS_NOT_DIRECTORY;
    case EACCES:
    case EPERM:
    case EROFS:
        return ESP_IRIS_FILE_STATUS_READ_ONLY;
    case ENOMEM:
        return ESP_IRIS_FILE_STATUS_NO_MEMORY;
    case EBUSY:
        return ESP_IRIS_FILE_STATUS_BUSY;
#ifdef EEXIST
    case EEXIST:
        return ESP_IRIS_FILE_STATUS_EXISTS;
#endif
#ifdef ENOTEMPTY
    case ENOTEMPTY:
        return ESP_IRIS_FILE_STATUS_NOT_EMPTY;
#endif
#ifdef ENOSPC
    case ENOSPC:
        return ESP_IRIS_FILE_STATUS_NO_SPACE;
#endif
#ifdef EXDEV
    case EXDEV:
        return ESP_IRIS_FILE_STATUS_NOT_SUPPORTED;
#endif
    default:
        return ESP_IRIS_FILE_STATUS_IO;
    }
}

static uint64_t weak_etag(const struct stat *metadata)
{
    uint64_t value = 1469598103934665603ULL;
    const uint64_t fields[] = {
        (uint64_t)metadata->st_mode,
        metadata->st_size < 0 ? 0 : (uint64_t)metadata->st_size,
        metadata->st_mtime < 0 ? 0 : (uint64_t)metadata->st_mtime,
    };
    const uint8_t *bytes = (const uint8_t *)fields;
    for (size_t i = 0; i < sizeof(fields); ++i) {
        value ^= bytes[i];
        value *= 1099511628211ULL;
    }
    return value;
}

static uint8_t file_kind(const struct stat *metadata)
{
    if (S_ISREG(metadata->st_mode)) {
        return ESP_IRIS_FILE_KIND_REGULAR;
    }
    if (S_ISDIR(metadata->st_mode)) {
        return ESP_IRIS_FILE_KIND_DIRECTORY;
    }
    return 0;
}

static esp_iris_file_status_t directory_is_empty(const char *path,
                                                 bool *out_empty)
{
    DIR *directory = opendir(path);
    if (directory == NULL) {
        return status_from_errno(errno);
    }
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            *out_empty = false;
            (void)closedir(directory);
            return ESP_IRIS_FILE_STATUS_OK;
        }
    }
    const int read_errno = errno;
    const int close_result = closedir(directory);
    if (read_errno != 0) {
        return status_from_errno(read_errno);
    }
    if (close_result != 0) {
        return status_from_errno(errno);
    }
    *out_empty = true;
    return ESP_IRIS_FILE_STATUS_OK;
}

static iris_file_volume_t *find_volume(const uint8_t *id, size_t size)
{
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_FILE_VOLUMES; ++i) {
        iris_file_volume_t *volume = &s_volumes[i];
        if (volume->used && strlen(volume->id) == size &&
            memcmp(volume->id, id, size) == 0) {
            return volume;
        }
    }
    return NULL;
}

static esp_iris_file_status_t decode_path_request_with_trailer(
    const iris_file_work_t *work, iris_file_volume_t **out_volume,
    char out_relative[ESP_IRIS_FILE_PATH_MAX + 1U], size_t trailer_size,
    const uint8_t **out_trailer)
{
    if (work->payload_size < IRIS_FILE_PATH_REQUEST_HEADER_SIZE) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    const uint8_t volume_size = work->payload[0];
    const uint16_t path_size = iris_get_le16(work->payload + 2);
    if (work->payload[1] != 0 || volume_size == 0 ||
        volume_size > ESP_IRIS_FILE_VOLUME_ID_MAX ||
        path_size > ESP_IRIS_FILE_PATH_MAX ||
        work->payload_size != IRIS_FILE_PATH_REQUEST_HEADER_SIZE +
                                  volume_size + path_size + trailer_size) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    iris_file_volume_t *volume = find_volume(
        work->payload + IRIS_FILE_PATH_REQUEST_HEADER_SIZE, volume_size);
    if (volume == NULL) {
        return ESP_IRIS_FILE_STATUS_NOT_FOUND;
    }
    const uint8_t *relative = work->payload +
        IRIS_FILE_PATH_REQUEST_HEADER_SIZE + volume_size;
    if (!valid_relative_path(relative, path_size)) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    memcpy(out_relative, relative, path_size);
    out_relative[path_size] = '\0';
    *out_volume = volume;
    if (out_trailer != NULL) {
        *out_trailer = relative + path_size;
    }
    return ESP_IRIS_FILE_STATUS_OK;
}

static esp_iris_file_status_t decode_path_request(
    const iris_file_work_t *work, iris_file_volume_t **out_volume,
    char out_relative[ESP_IRIS_FILE_PATH_MAX + 1U])
{
    return decode_path_request_with_trailer(work, out_volume, out_relative,
                                            0, NULL);
}

static esp_iris_file_status_t resolve_existing_path(
    const iris_file_volume_t *volume, const char *relative,
    char out[IRIS_FILE_FULL_PATH_SIZE], struct stat *out_metadata)
{
    const size_t base_size = strlen(volume->base_path);
    const size_t relative_size = strlen(relative);
    if (base_size + (relative_size > 0 ? 1U : 0U) + relative_size + 1U >
        IRIS_FILE_FULL_PATH_SIZE) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    memcpy(out, volume->base_path, base_size + 1U);
    if (relative_size == 0) {
        if (file_stat(out, out_metadata) != 0) {
            return status_from_errno(errno);
        }
        return S_ISLNK(out_metadata->st_mode)
            ? ESP_IRIS_FILE_STATUS_NOT_SUPPORTED
            : ESP_IRIS_FILE_STATUS_OK;
    }

    const char *component = relative;
    size_t used = base_size;
    while (*component != '\0') {
        const char *separator = strchr(component, '/');
        const size_t component_size = separator != NULL
            ? (size_t)(separator - component) : strlen(component);
        out[used++] = '/';
        memcpy(out + used, component, component_size);
        used += component_size;
        out[used] = '\0';
        if (file_stat(out, out_metadata) != 0) {
            return status_from_errno(errno);
        }
        if (S_ISLNK(out_metadata->st_mode)) {
            return ESP_IRIS_FILE_STATUS_NOT_SUPPORTED;
        }
        component = separator != NULL ? separator + 1 : component + component_size;
    }
    return ESP_IRIS_FILE_STATUS_OK;
}

static esp_iris_file_status_t resolve_parent_path(
    const iris_file_volume_t *volume, const char *relative,
    char out_parent[IRIS_FILE_FULL_PATH_SIZE],
    char out_target[IRIS_FILE_FULL_PATH_SIZE])
{
    if (relative[0] == '\0') {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    char parent_relative[ESP_IRIS_FILE_PATH_MAX + 1U];
    const char *separator = strrchr(relative, '/');
    const char *name = separator == NULL ? relative : separator + 1;
    const size_t parent_size = separator == NULL
        ? 0 : (size_t)(separator - relative);
    memcpy(parent_relative, relative, parent_size);
    parent_relative[parent_size] = '\0';
    struct stat metadata;
    esp_iris_file_status_t status = resolve_existing_path(
        volume, parent_relative, out_parent, &metadata);
    if (status != ESP_IRIS_FILE_STATUS_OK) {
        return status;
    }
    if (!S_ISDIR(metadata.st_mode)) {
        return ESP_IRIS_FILE_STATUS_NOT_DIRECTORY;
    }
    const int written = snprintf(out_target, IRIS_FILE_FULL_PATH_SIZE,
                                 "%s/%s", out_parent, name);
    return written < 0 || (size_t)written >= IRIS_FILE_FULL_PATH_SIZE
        ? ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT
        : ESP_IRIS_FILE_STATUS_OK;
}

static esp_iris_file_status_t target_metadata(
    const char *path, bool *out_exists, struct stat *out_metadata)
{
    if (file_stat(path, out_metadata) == 0) {
        *out_exists = true;
        return S_ISLNK(out_metadata->st_mode)
            ? ESP_IRIS_FILE_STATUS_NOT_SUPPORTED
            : ESP_IRIS_FILE_STATUS_OK;
    }
    if (errno == ENOENT) {
        *out_exists = false;
        memset(out_metadata, 0, sizeof(*out_metadata));
        return ESP_IRIS_FILE_STATUS_OK;
    }
    return status_from_errno(errno);
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right, size_t size)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

static void close_handle(iris_file_handle_t *handle)
{
    if (handle->directory != NULL) {
        closedir(handle->directory);
    }
    if (handle->fd >= 0) {
        close(handle->fd);
    }
    if (handle->hash_active) {
        (void)psa_hash_abort(&handle->hash);
    }
    if (handle->kind == IRIS_FILE_HANDLE_WRITE && handle->temp_path[0] != '\0') {
        (void)unlink(handle->temp_path);
    }
    memset(handle, 0, sizeof(*handle));
    handle->fd = -1;
}

static uint32_t next_stream_id(void)
{
    uint32_t result;
    do {
        result = esp_random();
    } while (result == 0);
    return result;
}

static void completion_begin(iris_file_completion_t *completion,
                             const iris_file_work_t *work, uint8_t response_type,
                             esp_iris_file_status_t status)
{
    memset(completion, 0, sizeof(*completion));
    completion->token = work->token;
    completion->type = response_type;
    completion->flags = ESP_IRIS_FLAG_RESPONSE;
    completion->session_id = work->session_id;
    completion->request_id = work->request_id;
    completion->stream_id = work->stream_id;
    completion->payload_size = 4;
    iris_put_le16(completion->payload, (uint16_t)status);
}

static uint8_t response_type_for(uint8_t request_type)
{
    switch (request_type) {
    case ESP_IRIS_FILE_VOLUMES_REQUEST:
        return ESP_IRIS_FILE_VOLUMES_RESPONSE;
    case ESP_IRIS_FILE_STAT_REQUEST:
        return ESP_IRIS_FILE_STAT_RESPONSE;
    case ESP_IRIS_FILE_LIST_OPEN:
        return ESP_IRIS_FILE_LIST_OPENED;
    case ESP_IRIS_FILE_LIST_NEXT:
        return ESP_IRIS_FILE_LIST_DATA;
    case ESP_IRIS_FILE_CLOSE:
        return ESP_IRIS_FILE_CLOSE_RESPONSE;
    case ESP_IRIS_FILE_READ_OPEN:
        return ESP_IRIS_FILE_READ_OPENED;
    case ESP_IRIS_FILE_READ:
        return ESP_IRIS_FILE_DATA;
    case ESP_IRIS_FILE_WRITE_OPEN:
        return ESP_IRIS_FILE_WRITE_OPENED;
    case ESP_IRIS_FILE_WRITE:
        return ESP_IRIS_FILE_WRITE_ACK;
    case ESP_IRIS_FILE_COMMIT:
        return ESP_IRIS_FILE_COMMIT_RESPONSE;
    case ESP_IRIS_FILE_ABORT:
        return ESP_IRIS_FILE_ABORT_RESPONSE;
    case ESP_IRIS_FILE_MKDIR:
        return ESP_IRIS_FILE_MKDIR_RESPONSE;
    case ESP_IRIS_FILE_DELETE:
        return ESP_IRIS_FILE_DELETE_RESPONSE;
    case ESP_IRIS_FILE_RENAME:
        return ESP_IRIS_FILE_RENAME_RESPONSE;
    case ESP_IRIS_FILE_WRITE_STATUS:
        return ESP_IRIS_FILE_WRITE_STATUS_RESPONSE;
    default:
        return request_type;
    }
}

static void handle_volumes(const iris_file_work_t *work,
                           iris_file_completion_t *completion)
{
    completion_begin(completion, work, ESP_IRIS_FILE_VOLUMES_RESPONSE,
                     ESP_IRIS_FILE_STATUS_OK);
    if (work->payload_size != 0) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    iris_put_le16(completion->payload + 4,
                  CONFIG_ESP_IRIS_FILE_CHUNK_BYTES);
    iris_put_le16(completion->payload + 6, ESP_IRIS_FILE_PATH_MAX);
    size_t offset = IRIS_FILE_VOLUME_RESPONSE_HEADER_SIZE;
    uint8_t count = 0;
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_FILE_VOLUMES; ++i) {
        const iris_file_volume_t *volume = &s_volumes[i];
        if (!volume->used) {
            continue;
        }
        const size_t id_size = strlen(volume->id);
        completion->payload[offset] = (uint8_t)id_size;
        completion->payload[offset + 1U] = 0;
        iris_put_le16(completion->payload + offset + 2U,
                      (uint16_t)volume->capabilities);
        memcpy(completion->payload + offset + 4U, volume->id, id_size);
        offset += 4U + id_size;
        ++count;
    }
    completion->payload[8] = count;
    completion->payload_size = (uint16_t)offset;
}

static void encode_metadata(uint8_t out[28], const struct stat *metadata)
{
    out[0] = file_kind(metadata);
    out[1] = 0;
    iris_put_le16(out + 2, 0);
    iris_put_le64(out + 4, metadata->st_size < 0 ? 0 :
                  (uint64_t)metadata->st_size);
    iris_put_le64(out + 12, metadata->st_mtime < 0 ? 0 :
                  (uint64_t)metadata->st_mtime);
    iris_put_le64(out + 20, weak_etag(metadata));
}

static void handle_stat(const iris_file_work_t *work,
                        iris_file_completion_t *completion)
{
    completion_begin(completion, work, ESP_IRIS_FILE_STAT_RESPONSE,
                     ESP_IRIS_FILE_STATUS_OK);
    iris_file_volume_t *volume = NULL;
    char relative[ESP_IRIS_FILE_PATH_MAX + 1U];
    esp_iris_file_status_t status = decode_path_request(work, &volume, relative);
    char full_path[IRIS_FILE_FULL_PATH_SIZE];
    struct stat metadata;
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = resolve_existing_path(volume, relative, full_path, &metadata);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && file_kind(&metadata) == 0) {
        status = ESP_IRIS_FILE_STATUS_NOT_SUPPORTED;
    }
    iris_put_le16(completion->payload, (uint16_t)status);
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        encode_metadata(completion->payload + 4, &metadata);
        completion->payload_size = 32;
    }
}

static void handle_list_open(const iris_file_work_t *work,
                             iris_file_completion_t *completion,
                             iris_file_handle_t *handle)
{
    completion_begin(completion, work, ESP_IRIS_FILE_LIST_OPENED,
                     ESP_IRIS_FILE_STATUS_OK);
    if (handle->kind != IRIS_FILE_HANDLE_NONE) {
        iris_put_le16(completion->payload, ESP_IRIS_FILE_STATUS_BUSY);
        return;
    }
    iris_file_volume_t *volume = NULL;
    char relative[ESP_IRIS_FILE_PATH_MAX + 1U];
    esp_iris_file_status_t status = decode_path_request(work, &volume, relative);
    struct stat metadata;
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        !(volume->capabilities & ESP_IRIS_FILE_VOLUME_LIST)) {
        status = ESP_IRIS_FILE_STATUS_READ_ONLY;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = resolve_existing_path(volume, relative, handle->directory_path,
                                       &metadata);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && !S_ISDIR(metadata.st_mode)) {
        status = ESP_IRIS_FILE_STATUS_NOT_DIRECTORY;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        handle->directory = opendir(handle->directory_path);
        if (handle->directory == NULL) {
            status = status_from_errno(errno);
        }
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        handle->kind = IRIS_FILE_HANDLE_LIST;
        handle->session_id = work->session_id;
        handle->stream_id = next_stream_id();
        completion->stream_id = handle->stream_id;
        completion->flags |= ESP_IRIS_FLAG_STREAM_BEGIN;
        iris_put_le32(completion->payload + 4, handle->stream_id);
        completion->payload_size = 8;
    } else {
        iris_put_le16(completion->payload, (uint16_t)status);
        close_handle(handle);
    }
}

static void handle_list_next(const iris_file_work_t *work,
                             iris_file_completion_t *completion,
                             iris_file_handle_t *handle)
{
    completion_begin(completion, work, ESP_IRIS_FILE_LIST_DATA,
                     ESP_IRIS_FILE_STATUS_OK);
    completion->stream_id = handle->stream_id;
    if (work->payload_size != 0 || handle->kind != IRIS_FILE_HANDLE_LIST ||
        handle->session_id != work->session_id ||
        handle->stream_id != work->stream_id) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    struct dirent *entry = NULL;
    struct stat metadata;
    char child_path[IRIS_FILE_FULL_PATH_SIZE];
    while ((entry = readdir(handle->directory)) != NULL) {
        const size_t name_size = strnlen(entry->d_name, ESP_IRIS_FILE_PATH_MAX + 1U);
        if (name_size == 0 || name_size > ESP_IRIS_FILE_PATH_MAX ||
            strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            !valid_utf8((const uint8_t *)entry->d_name, name_size)) {
            continue;
        }
        const int written = snprintf(child_path, sizeof(child_path), "%s/%s",
                                     handle->directory_path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(child_path) ||
            file_stat(child_path, &metadata) != 0 || S_ISLNK(metadata.st_mode) ||
            file_kind(&metadata) == 0) {
            continue;
        }
        completion->payload[2] = 0;
        completion->payload[3] = 1;
        encode_metadata(completion->payload + IRIS_FILE_LIST_DATA_HEADER_SIZE,
                        &metadata);
        completion->payload[IRIS_FILE_LIST_DATA_HEADER_SIZE + 1U] =
            (uint8_t)name_size;
        memcpy(completion->payload + IRIS_FILE_LIST_DATA_HEADER_SIZE +
                   IRIS_FILE_LIST_ENTRY_HEADER_SIZE,
               entry->d_name, name_size);
        completion->payload_size = (uint16_t)(
            IRIS_FILE_LIST_DATA_HEADER_SIZE + IRIS_FILE_LIST_ENTRY_HEADER_SIZE +
            name_size);
        return;
    }
    completion->payload[2] = IRIS_FILE_LIST_END;
    completion->payload[3] = 0;
    completion->flags |= ESP_IRIS_FLAG_STREAM_END;
}

static void handle_read_open(const iris_file_work_t *work,
                             iris_file_completion_t *completion,
                             iris_file_handle_t *handle)
{
    completion_begin(completion, work, ESP_IRIS_FILE_READ_OPENED,
                     ESP_IRIS_FILE_STATUS_OK);
    if (handle->kind != IRIS_FILE_HANDLE_NONE) {
        iris_put_le16(completion->payload, ESP_IRIS_FILE_STATUS_BUSY);
        return;
    }
    iris_file_volume_t *volume = NULL;
    char relative[ESP_IRIS_FILE_PATH_MAX + 1U];
    esp_iris_file_status_t status = decode_path_request(work, &volume, relative);
    char full_path[IRIS_FILE_FULL_PATH_SIZE];
    struct stat metadata;
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        !(volume->capabilities & ESP_IRIS_FILE_VOLUME_READ)) {
        status = ESP_IRIS_FILE_STATUS_READ_ONLY;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = resolve_existing_path(volume, relative, full_path, &metadata);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && !S_ISREG(metadata.st_mode)) {
        status = ESP_IRIS_FILE_STATUS_NOT_FILE;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        handle->fd = open(full_path, O_RDONLY);
        if (handle->fd < 0 || fstat(handle->fd, &metadata) != 0) {
            status = status_from_errno(errno);
        }
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        handle->kind = IRIS_FILE_HANDLE_READ;
        handle->session_id = work->session_id;
        handle->stream_id = next_stream_id();
        handle->size = metadata.st_size < 0 ? 0 : (uint64_t)metadata.st_size;
        completion->stream_id = handle->stream_id;
        completion->flags |= ESP_IRIS_FLAG_STREAM_BEGIN;
        iris_put_le32(completion->payload + 4, handle->stream_id);
        iris_put_le64(completion->payload + 8, handle->size);
        iris_put_le64(completion->payload + 16,
                      metadata.st_mtime < 0 ? 0 : (uint64_t)metadata.st_mtime);
        iris_put_le64(completion->payload + 24, weak_etag(&metadata));
        iris_put_le16(completion->payload + 32,
                      CONFIG_ESP_IRIS_FILE_CHUNK_BYTES);
        completion->payload_size = 36;
    } else {
        iris_put_le16(completion->payload, (uint16_t)status);
        close_handle(handle);
    }
}

static void handle_read(const iris_file_work_t *work,
                        iris_file_completion_t *completion,
                        iris_file_handle_t *handle)
{
    completion_begin(completion, work, ESP_IRIS_FILE_DATA,
                     ESP_IRIS_FILE_STATUS_OK);
    completion->stream_id = handle->stream_id;
    if (work->payload_size != 12 || handle->kind != IRIS_FILE_HANDLE_READ ||
        handle->session_id != work->session_id ||
        handle->stream_id != work->stream_id ||
        iris_get_le16(work->payload + 10) != 0) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    const uint64_t offset = iris_get_le64(work->payload);
    uint16_t maximum = iris_get_le16(work->payload + 8);
    if (maximum > CONFIG_ESP_IRIS_FILE_CHUNK_BYTES) {
        maximum = CONFIG_ESP_IRIS_FILE_CHUNK_BYTES;
    }
    const off_t seek_offset = (off_t)offset;
    if (maximum == 0 || offset >= handle->size || seek_offset < 0 ||
        (uint64_t)seek_offset != offset ||
        lseek(handle->fd, seek_offset, SEEK_SET) < 0) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    const ssize_t result = read(handle->fd,
                                completion->payload + IRIS_FILE_READ_DATA_HEADER_SIZE,
                                maximum);
    if (result <= 0) {
        iris_put_le16(completion->payload,
                      result == 0 ? ESP_IRIS_FILE_STATUS_CONFLICT
                                  : status_from_errno(errno));
        return;
    }
    iris_put_le64(completion->payload + 4, offset);
    iris_put_le64(completion->payload + 12, handle->size);
    completion->payload_size = (uint16_t)(IRIS_FILE_READ_DATA_HEADER_SIZE + result);
    if (offset + (uint64_t)result == handle->size) {
        completion->flags |= ESP_IRIS_FLAG_STREAM_END;
        iris_put_le16(completion->payload + 2, IRIS_FILE_LIST_END);
    }
}

static void set_write_receipt(iris_file_write_receipt_t *receipt,
                              const iris_file_handle_t *handle,
                              uint8_t state,
                              esp_iris_file_status_t result)
{
    *receipt = (iris_file_write_receipt_t) {
        .valid = true,
        .session_id = handle->session_id,
        .stream_id = handle->stream_id,
        .committed = handle->committed,
        .expected = handle->size,
        .state = state,
        .result = result,
    };
}

static void handle_write_open(const iris_file_work_t *work,
                              iris_file_completion_t *completion,
                              iris_file_handle_t *handle,
                              iris_file_write_receipt_t *receipt)
{
    completion_begin(completion, work, ESP_IRIS_FILE_WRITE_OPENED,
                     ESP_IRIS_FILE_STATUS_OK);
    if (handle->kind != IRIS_FILE_HANDLE_NONE) {
        iris_put_le16(completion->payload, ESP_IRIS_FILE_STATUS_BUSY);
        return;
    }
    iris_file_volume_t *volume = NULL;
    char relative[ESP_IRIS_FILE_PATH_MAX + 1U];
    const uint8_t *trailer = NULL;
    esp_iris_file_status_t status = decode_path_request_with_trailer(
        work, &volume, relative, IRIS_FILE_WRITE_OPEN_TRAILER_SIZE, &trailer);
    uint64_t expected_size = 0;
    uint64_t if_match = 0;
    uint16_t flags = 0;
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        expected_size = iris_get_le64(trailer);
        if_match = iris_get_le64(trailer + 8);
        flags = iris_get_le16(trailer + 16);
        const off_t expected_offset = (off_t)expected_size;
        if (iris_get_le16(trailer + 18) != 0 ||
            (flags & ~(ESP_IRIS_FILE_WRITE_OVERWRITE |
                       ESP_IRIS_FILE_WRITE_IF_MATCH)) != 0 ||
            expected_offset < 0 || (uint64_t)expected_offset != expected_size) {
            status = ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        !(volume->capabilities & ESP_IRIS_FILE_VOLUME_WRITE)) {
        status = ESP_IRIS_FILE_STATUS_READ_ONLY;
    }
    char parent_path[IRIS_FILE_FULL_PATH_SIZE];
    struct stat metadata;
    bool exists = false;
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = resolve_parent_path(volume, relative, parent_path,
                                     handle->target_path);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = target_metadata(handle->target_path, &exists, &metadata);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && exists &&
        !S_ISREG(metadata.st_mode)) {
        status = ESP_IRIS_FILE_STATUS_NOT_FILE;
    }
    const bool overwrite = (flags & ESP_IRIS_FILE_WRITE_OVERWRITE) != 0;
    if (status == ESP_IRIS_FILE_STATUS_OK && exists && !overwrite) {
        status = ESP_IRIS_FILE_STATUS_EXISTS;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && exists && overwrite &&
        !(volume->capabilities & ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE)) {
        status = ESP_IRIS_FILE_STATUS_NOT_SUPPORTED;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        (flags & ESP_IRIS_FILE_WRITE_IF_MATCH) != 0 &&
        (!exists || weak_etag(&metadata) != if_match)) {
        status = ESP_IRIS_FILE_STATUS_CONFLICT;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        for (size_t attempt = 0; attempt < 8; ++attempt) {
            const int written = snprintf(
                handle->temp_path, sizeof(handle->temp_path),
                "%s/.iris-%08" PRIx32 ".tmp", parent_path, esp_random());
            if (written < 0 || (size_t)written >= sizeof(handle->temp_path)) {
                status = ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
                break;
            }
            handle->fd = open(handle->temp_path,
                              O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (handle->fd >= 0) {
                break;
            }
            if (errno != EEXIST) {
                status = status_from_errno(errno);
                break;
            }
        }
        if (handle->fd < 0 && status == ESP_IRIS_FILE_STATUS_OK) {
            status = ESP_IRIS_FILE_STATUS_EXISTS;
        }
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        handle->kind = IRIS_FILE_HANDLE_WRITE;
        handle->session_id = work->session_id;
        handle->stream_id = next_stream_id();
        handle->size = expected_size;
        handle->target_existed = exists;
        handle->overwrite = overwrite;
        handle->original_etag = exists ? weak_etag(&metadata) : 0;
        handle->hash = psa_hash_operation_init();
        handle->hash_active = psa_crypto_init() == PSA_SUCCESS &&
            psa_hash_setup(&handle->hash, PSA_ALG_SHA_256) == PSA_SUCCESS;
        if (!handle->hash_active) {
            status = ESP_IRIS_FILE_STATUS_IO;
        }
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        receipt->valid = false;
        completion->stream_id = handle->stream_id;
        completion->flags |= ESP_IRIS_FLAG_STREAM_BEGIN;
        iris_put_le32(completion->payload + 4, handle->stream_id);
        iris_put_le16(completion->payload + 8,
                      CONFIG_ESP_IRIS_FILE_CHUNK_BYTES);
        completion->payload_size = 12;
    } else {
        iris_put_le16(completion->payload, (uint16_t)status);
        close_handle(handle);
    }
}

static void handle_write(const iris_file_work_t *work,
                         iris_file_completion_t *completion,
                         iris_file_handle_t *handle,
                         iris_file_write_receipt_t *receipt)
{
    completion_begin(completion, work, ESP_IRIS_FILE_WRITE_ACK,
                     ESP_IRIS_FILE_STATUS_OK);
    completion->stream_id = work->stream_id;
    if (work->payload_size <= IRIS_FILE_WRITE_DATA_HEADER_SIZE ||
        handle->kind != IRIS_FILE_HANDLE_WRITE ||
        handle->session_id != work->session_id ||
        handle->stream_id != work->stream_id) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    const uint64_t offset = iris_get_le64(work->payload);
    const uint16_t data_size = iris_get_le16(work->payload + 8);
    if (iris_get_le16(work->payload + 10) != 0 ||
        data_size == 0 || data_size > CONFIG_ESP_IRIS_FILE_CHUNK_BYTES ||
        work->payload_size != IRIS_FILE_WRITE_DATA_HEADER_SIZE + data_size ||
        offset != handle->committed ||
        handle->committed > handle->size ||
        data_size > handle->size - handle->committed) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    const uint8_t *data = work->payload + IRIS_FILE_WRITE_DATA_HEADER_SIZE;
    size_t written_total = 0;
    esp_iris_file_status_t status = ESP_IRIS_FILE_STATUS_OK;
    while (written_total < data_size) {
        const ssize_t written = write(handle->fd, data + written_total,
                                      data_size - written_total);
        if (written <= 0) {
            status = status_from_errno(errno);
            break;
        }
        written_total += (size_t)written;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        psa_hash_update(&handle->hash, data, data_size) != PSA_SUCCESS) {
        status = ESP_IRIS_FILE_STATUS_IO;
    }
    if (status != ESP_IRIS_FILE_STATUS_OK) {
        set_write_receipt(receipt, handle, IRIS_FILE_WRITE_ABORTED, status);
        close_handle(handle);
        iris_put_le16(completion->payload, (uint16_t)status);
        completion->flags |= ESP_IRIS_FLAG_STREAM_END;
        return;
    }
    handle->committed += data_size;
    iris_put_le64(completion->payload + 4, handle->committed);
    completion->payload_size = 12;
}

static void handle_commit(const iris_file_work_t *work,
                          iris_file_completion_t *completion,
                          iris_file_handle_t *handle,
                          iris_file_write_receipt_t *receipt)
{
    completion_begin(completion, work, ESP_IRIS_FILE_COMMIT_RESPONSE,
                     ESP_IRIS_FILE_STATUS_OK);
    completion->stream_id = work->stream_id;
    if (work->payload_size != 32 ||
        handle->kind != IRIS_FILE_HANDLE_WRITE ||
        handle->session_id != work->session_id ||
        handle->stream_id != work->stream_id) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    esp_iris_file_status_t status = ESP_IRIS_FILE_STATUS_OK;
    uint8_t digest[32];
    size_t digest_size = 0;
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        handle->committed != handle->size) {
        status = ESP_IRIS_FILE_STATUS_CONFLICT;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        if (psa_hash_finish(&handle->hash, digest, sizeof(digest),
                            &digest_size) == PSA_SUCCESS) {
            handle->hash_active = false;
        } else {
            status = ESP_IRIS_FILE_STATUS_IO;
        }
    }
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        (digest_size != sizeof(digest) ||
         !bytes_equal(digest, work->payload, sizeof(digest)))) {
        status = ESP_IRIS_FILE_STATUS_HASH_MISMATCH;
    }
    struct stat metadata;
    bool exists = false;
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = target_metadata(handle->target_path, &exists, &metadata);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        (exists != handle->target_existed ||
         (exists && weak_etag(&metadata) != handle->original_etag))) {
        status = ESP_IRIS_FILE_STATUS_CONFLICT;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && fsync(handle->fd) != 0) {
        status = status_from_errno(errno);
    }
    if (handle->fd >= 0) {
        if (close(handle->fd) != 0 && status == ESP_IRIS_FILE_STATUS_OK) {
            status = status_from_errno(errno);
        }
        handle->fd = -1;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        rename(handle->temp_path, handle->target_path) != 0) {
        status = status_from_errno(errno);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        handle->temp_path[0] = '\0';
        if (file_stat(handle->target_path, &metadata) != 0) {
            status = status_from_errno(errno);
        }
    }
    set_write_receipt(receipt, handle,
                      status == ESP_IRIS_FILE_STATUS_OK
                          ? IRIS_FILE_WRITE_COMMITTED
                          : IRIS_FILE_WRITE_ABORTED,
                      status);
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        encode_metadata(completion->payload + 4, &metadata);
        completion->payload_size = 32;
    } else {
        iris_put_le16(completion->payload, (uint16_t)status);
    }
    close_handle(handle);
    completion->flags |= ESP_IRIS_FLAG_STREAM_END;
}

static void handle_abort(const iris_file_work_t *work,
                         iris_file_completion_t *completion,
                         iris_file_handle_t *handle,
                         iris_file_write_receipt_t *receipt)
{
    completion_begin(completion, work, ESP_IRIS_FILE_ABORT_RESPONSE,
                     ESP_IRIS_FILE_STATUS_OK);
    completion->stream_id = work->stream_id;
    if (work->payload_size != 0 ||
        handle->kind != IRIS_FILE_HANDLE_WRITE ||
        handle->session_id != work->session_id ||
        handle->stream_id != work->stream_id) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    set_write_receipt(receipt, handle, IRIS_FILE_WRITE_ABORTED,
                      ESP_IRIS_FILE_STATUS_CONFLICT);
    close_handle(handle);
    completion->flags |= ESP_IRIS_FLAG_STREAM_END;
}

static void handle_write_status(const iris_file_work_t *work,
                                iris_file_completion_t *completion,
                                const iris_file_handle_t *handle,
                                const iris_file_write_receipt_t *receipt)
{
    completion_begin(completion, work, ESP_IRIS_FILE_WRITE_STATUS_RESPONSE,
                     ESP_IRIS_FILE_STATUS_OK);
    completion->stream_id = work->stream_id;
    uint64_t committed = 0;
    uint64_t expected = 0;
    uint8_t state = 0;
    esp_iris_file_status_t result = ESP_IRIS_FILE_STATUS_OK;
    if (work->payload_size != 0) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    if (handle->kind == IRIS_FILE_HANDLE_WRITE &&
        handle->session_id == work->session_id &&
        handle->stream_id == work->stream_id) {
        committed = handle->committed;
        expected = handle->size;
        state = IRIS_FILE_WRITE_ACTIVE;
    } else if (receipt->valid && receipt->session_id == work->session_id &&
               receipt->stream_id == work->stream_id) {
        committed = receipt->committed;
        expected = receipt->expected;
        state = receipt->state;
        result = receipt->result;
    } else {
        iris_put_le16(completion->payload, ESP_IRIS_FILE_STATUS_NOT_FOUND);
        return;
    }
    iris_put_le64(completion->payload + 4, committed);
    iris_put_le64(completion->payload + 12, expected);
    completion->payload[20] = state;
    iris_put_le16(completion->payload + 24, (uint16_t)result);
    completion->payload_size = 28;
}

static void handle_mkdir(const iris_file_work_t *work,
                         iris_file_completion_t *completion,
                         const iris_file_handle_t *handle)
{
    completion_begin(completion, work, ESP_IRIS_FILE_MKDIR_RESPONSE,
                     ESP_IRIS_FILE_STATUS_OK);
    iris_file_volume_t *volume = NULL;
    char relative[ESP_IRIS_FILE_PATH_MAX + 1U];
    esp_iris_file_status_t status = handle->kind == IRIS_FILE_HANDLE_NONE
        ? decode_path_request(work, &volume, relative)
        : ESP_IRIS_FILE_STATUS_BUSY;
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        !(volume->capabilities & ESP_IRIS_FILE_VOLUME_MKDIR)) {
        status = ESP_IRIS_FILE_STATUS_READ_ONLY;
    }
    char parent[IRIS_FILE_FULL_PATH_SIZE];
    char target[IRIS_FILE_FULL_PATH_SIZE];
    struct stat metadata;
    bool exists = false;
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = resolve_parent_path(volume, relative, parent, target);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = target_metadata(target, &exists, &metadata);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && exists) {
        status = ESP_IRIS_FILE_STATUS_EXISTS;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && mkdir(target, 0755) != 0) {
        status = status_from_errno(errno);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && file_stat(target, &metadata) != 0) {
        status = status_from_errno(errno);
    }
    iris_put_le16(completion->payload, (uint16_t)status);
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        encode_metadata(completion->payload + 4, &metadata);
        completion->payload_size = 32;
    }
}

static void handle_delete(const iris_file_work_t *work,
                          iris_file_completion_t *completion,
                          const iris_file_handle_t *handle)
{
    completion_begin(completion, work, ESP_IRIS_FILE_DELETE_RESPONSE,
                     ESP_IRIS_FILE_STATUS_OK);
    iris_file_volume_t *volume = NULL;
    char relative[ESP_IRIS_FILE_PATH_MAX + 1U];
    esp_iris_file_status_t status = handle->kind == IRIS_FILE_HANDLE_NONE
        ? decode_path_request(work, &volume, relative)
        : ESP_IRIS_FILE_STATUS_BUSY;
    if (status == ESP_IRIS_FILE_STATUS_OK && relative[0] == '\0') {
        status = ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        !(volume->capabilities & ESP_IRIS_FILE_VOLUME_DELETE)) {
        status = ESP_IRIS_FILE_STATUS_READ_ONLY;
    }
    char target[IRIS_FILE_FULL_PATH_SIZE];
    struct stat metadata;
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = resolve_existing_path(volume, relative, target, &metadata);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        if (S_ISDIR(metadata.st_mode)) {
            bool empty = false;
            status = directory_is_empty(target, &empty);
            if (status == ESP_IRIS_FILE_STATUS_OK && !empty) {
                status = ESP_IRIS_FILE_STATUS_NOT_EMPTY;
            }
            if (status == ESP_IRIS_FILE_STATUS_OK && rmdir(target) != 0) {
                status = status_from_errno(errno);
            }
        } else if (S_ISREG(metadata.st_mode)) {
            if (unlink(target) != 0) {
                status = status_from_errno(errno);
            }
        } else {
            status = ESP_IRIS_FILE_STATUS_NOT_SUPPORTED;
        }
    }
    iris_put_le16(completion->payload, (uint16_t)status);
}

static esp_iris_file_status_t decode_rename_request(
    const iris_file_work_t *work, iris_file_volume_t **out_volume,
    char source[ESP_IRIS_FILE_PATH_MAX + 1U],
    char destination[ESP_IRIS_FILE_PATH_MAX + 1U])
{
    if (work->payload_size < IRIS_FILE_RENAME_HEADER_SIZE) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    const uint8_t volume_size = work->payload[0];
    const uint16_t source_size = iris_get_le16(work->payload + 2);
    const uint16_t destination_size = iris_get_le16(work->payload + 4);
    if (work->payload[1] != 0 || iris_get_le16(work->payload + 6) != 0 ||
        volume_size == 0 || volume_size > ESP_IRIS_FILE_VOLUME_ID_MAX ||
        source_size == 0 || source_size > ESP_IRIS_FILE_PATH_MAX ||
        destination_size == 0 || destination_size > ESP_IRIS_FILE_PATH_MAX ||
        work->payload_size != IRIS_FILE_RENAME_HEADER_SIZE + volume_size +
                                  source_size + destination_size) {
        return ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    iris_file_volume_t *volume = find_volume(
        work->payload + IRIS_FILE_RENAME_HEADER_SIZE, volume_size);
    const uint8_t *source_bytes = work->payload + IRIS_FILE_RENAME_HEADER_SIZE +
        volume_size;
    const uint8_t *destination_bytes = source_bytes + source_size;
    if (volume == NULL || !valid_relative_path(source_bytes, source_size) ||
        !valid_relative_path(destination_bytes, destination_size)) {
        return volume == NULL ? ESP_IRIS_FILE_STATUS_NOT_FOUND
                              : ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
    }
    memcpy(source, source_bytes, source_size);
    source[source_size] = '\0';
    memcpy(destination, destination_bytes, destination_size);
    destination[destination_size] = '\0';
    *out_volume = volume;
    return strcmp(source, destination) == 0
        ? ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT
        : ESP_IRIS_FILE_STATUS_OK;
}

static void handle_rename(const iris_file_work_t *work,
                          iris_file_completion_t *completion,
                          const iris_file_handle_t *handle)
{
    completion_begin(completion, work, ESP_IRIS_FILE_RENAME_RESPONSE,
                     ESP_IRIS_FILE_STATUS_OK);
    iris_file_volume_t *volume = NULL;
    char source[ESP_IRIS_FILE_PATH_MAX + 1U];
    char destination[ESP_IRIS_FILE_PATH_MAX + 1U];
    esp_iris_file_status_t status = handle->kind == IRIS_FILE_HANDLE_NONE
        ? decode_rename_request(work, &volume, source, destination)
        : ESP_IRIS_FILE_STATUS_BUSY;
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        !(volume->capabilities & ESP_IRIS_FILE_VOLUME_RENAME)) {
        status = ESP_IRIS_FILE_STATUS_READ_ONLY;
    }
    char source_path[IRIS_FILE_FULL_PATH_SIZE];
    char parent[IRIS_FILE_FULL_PATH_SIZE];
    char destination_path[IRIS_FILE_FULL_PATH_SIZE];
    struct stat metadata;
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = resolve_existing_path(volume, source, source_path, &metadata);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        const size_t source_size = strlen(source);
        const size_t destination_size = strlen(destination);
        if (S_ISDIR(metadata.st_mode) && destination_size > source_size &&
            strncmp(destination, source, source_size) == 0 &&
            destination[source_size] == '/') {
            status = ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT;
        }
    }
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = resolve_parent_path(volume, destination, parent,
                                     destination_path);
    }
    bool exists = false;
    struct stat destination_metadata;
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        status = target_metadata(destination_path, &exists,
                                 &destination_metadata);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK && exists) {
        status = ESP_IRIS_FILE_STATUS_EXISTS;
    }
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        rename(source_path, destination_path) != 0) {
        status = status_from_errno(errno);
    }
    if (status == ESP_IRIS_FILE_STATUS_OK &&
        file_stat(destination_path, &metadata) != 0) {
        status = status_from_errno(errno);
    }
    iris_put_le16(completion->payload, (uint16_t)status);
    if (status == ESP_IRIS_FILE_STATUS_OK) {
        encode_metadata(completion->payload + 4, &metadata);
        completion->payload_size = 32;
    }
}

static void handle_close(const iris_file_work_t *work,
                         iris_file_completion_t *completion,
                         iris_file_handle_t *handle)
{
    completion_begin(completion, work, ESP_IRIS_FILE_CLOSE_RESPONSE,
                     ESP_IRIS_FILE_STATUS_OK);
    completion->stream_id = work->stream_id;
    if (work->payload_size != 0 || handle->kind == IRIS_FILE_HANDLE_NONE ||
        handle->kind == IRIS_FILE_HANDLE_WRITE ||
        handle->session_id != work->session_id ||
        handle->stream_id != work->stream_id) {
        iris_put_le16(completion->payload,
                      ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return;
    }
    close_handle(handle);
    completion->flags |= ESP_IRIS_FLAG_STREAM_END;
}

static void process_work(const iris_file_work_t *work,
                         iris_file_completion_t *completion,
                         iris_file_handle_t *handle,
                         iris_file_write_receipt_t *receipt)
{
    switch (work->type) {
    case ESP_IRIS_FILE_VOLUMES_REQUEST:
        handle_volumes(work, completion);
        break;
    case ESP_IRIS_FILE_STAT_REQUEST:
        handle_stat(work, completion);
        break;
    case ESP_IRIS_FILE_LIST_OPEN:
        handle_list_open(work, completion, handle);
        break;
    case ESP_IRIS_FILE_LIST_NEXT:
        handle_list_next(work, completion, handle);
        break;
    case ESP_IRIS_FILE_READ_OPEN:
        handle_read_open(work, completion, handle);
        break;
    case ESP_IRIS_FILE_READ:
        handle_read(work, completion, handle);
        break;
    case ESP_IRIS_FILE_WRITE_OPEN:
        handle_write_open(work, completion, handle, receipt);
        break;
    case ESP_IRIS_FILE_WRITE:
        handle_write(work, completion, handle, receipt);
        break;
    case ESP_IRIS_FILE_COMMIT:
        handle_commit(work, completion, handle, receipt);
        break;
    case ESP_IRIS_FILE_ABORT:
        handle_abort(work, completion, handle, receipt);
        break;
    case ESP_IRIS_FILE_MKDIR:
        handle_mkdir(work, completion, handle);
        break;
    case ESP_IRIS_FILE_DELETE:
        handle_delete(work, completion, handle);
        break;
    case ESP_IRIS_FILE_RENAME:
        handle_rename(work, completion, handle);
        break;
    case ESP_IRIS_FILE_WRITE_STATUS:
        handle_write_status(work, completion, handle, receipt);
        break;
    case ESP_IRIS_FILE_CLOSE:
        handle_close(work, completion, handle);
        break;
    default:
        completion_begin(completion, work, response_type_for(work->type),
                         ESP_IRIS_FILE_STATUS_NOT_SUPPORTED);
        break;
    }
}

static void file_task(void *argument)
{
    (void)argument;
    memset(&s_task_handle, 0, sizeof(s_task_handle));
    s_task_handle.fd = -1;
    memset(&s_write_receipt, 0, sizeof(s_write_receipt));
    while (xQueueReceive(s_work_queue, &s_task_work, portMAX_DELAY) == pdTRUE) {
        if (s_task_work.kind == IRIS_FILE_WORK_STOP) {
            break;
        }
        if (s_task_work.kind == IRIS_FILE_WORK_SESSION_END) {
            if (s_task_handle.kind != IRIS_FILE_HANDLE_NONE &&
                s_task_handle.session_id == s_task_work.session_id) {
                close_handle(&s_task_handle);
            }
            if (s_write_receipt.valid &&
                s_write_receipt.session_id == s_task_work.session_id) {
                s_write_receipt.valid = false;
            }
            continue;
        }
        process_work(&s_task_work, &s_task_completion, &s_task_handle,
                     &s_write_receipt);
        (void)xQueueSend(s_completion_queue, &s_task_completion, 0);
        if (g_iris.task != NULL) {
            xTaskNotifyGive(g_iris.task);
        }
    }
    close_handle(&s_task_handle);
    s_file_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t esp_iris_file_volume_register(
    const esp_iris_file_volume_config_t *config)
{
    const uint32_t allowed = ESP_IRIS_FILE_VOLUME_READ |
        ESP_IRIS_FILE_VOLUME_LIST | ESP_IRIS_FILE_VOLUME_MTIME |
        ESP_IRIS_FILE_VOLUME_WRITE | ESP_IRIS_FILE_VOLUME_DELETE |
        ESP_IRIS_FILE_VOLUME_MKDIR | ESP_IRIS_FILE_VOLUME_RENAME |
        ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE | ESP_IRIS_FILE_VOLUME_HASH;
    if (config == NULL || !valid_volume_id(config->id) ||
        config->base_path == NULL || config->base_path[0] != '/' ||
        strcmp(config->base_path, "/") == 0 ||
        config->capabilities == 0 || (config->capabilities & ~allowed) != 0 ||
        ((config->capabilities & (ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE |
                                  ESP_IRIS_FILE_VOLUME_HASH)) != 0 &&
         (config->capabilities & ESP_IRIS_FILE_VOLUME_WRITE) == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_iris_is_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t base_size = strnlen(config->base_path,
                                     ESP_IRIS_FILE_PATH_MAX + 1U);
    if (base_size == 0 || base_size > ESP_IRIS_FILE_PATH_MAX ||
        config->base_path[base_size - 1U] == '/') {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat metadata;
    if (file_stat(config->base_path, &metadata) != 0 ||
        !S_ISDIR(metadata.st_mode) || S_ISLNK(metadata.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    taskENTER_CRITICAL(&s_volume_lock);
    iris_file_volume_t *slot = NULL;
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_FILE_VOLUMES; ++i) {
        if (s_volumes[i].used && strcmp(s_volumes[i].id, config->id) == 0) {
            taskEXIT_CRITICAL(&s_volume_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (!s_volumes[i].used && slot == NULL) {
            slot = &s_volumes[i];
        }
    }
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->id, config->id, strlen(config->id) + 1U);
        memcpy(slot->base_path, config->base_path, base_size + 1U);
        slot->capabilities = config->capabilities |
            ((config->capabilities & ESP_IRIS_FILE_VOLUME_WRITE) != 0
                 ? ESP_IRIS_FILE_VOLUME_HASH : 0);
        slot->used = true;
    }
    taskEXIT_CRITICAL(&s_volume_lock);
    return slot == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t esp_iris_file_volume_unregister(const char *id)
{
    if (!valid_volume_id(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_iris_is_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_volume_lock);
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_FILE_VOLUMES; ++i) {
        if (s_volumes[i].used && strcmp(s_volumes[i].id, id) == 0) {
            memset(&s_volumes[i], 0, sizeof(s_volumes[i]));
            taskEXIT_CRITICAL(&s_volume_lock);
            return ESP_OK;
        }
    }
    taskEXIT_CRITICAL(&s_volume_lock);
    return ESP_ERR_NOT_FOUND;
}

uint64_t iris_files_capabilities(void)
{
    for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_FILE_VOLUMES; ++i) {
        if (s_volumes[i].used) {
            return ESP_IRIS_CAP_FILE;
        }
    }
    return 0;
}

esp_err_t iris_files_init(iris_runtime_t *runtime)
{
    (void)runtime;
    if (iris_files_capabilities() == 0) {
        return ESP_OK;
    }
    if (s_file_task != NULL) {
        return ESP_OK;
    }
    atomic_store(&s_active_token, 0);
    s_work_queue = xQueueCreate(1, sizeof(iris_file_work_t));
    s_completion_queue = xQueueCreate(1, sizeof(iris_file_completion_t));
    if (s_work_queue == NULL || s_completion_queue == NULL) {
        iris_files_deinit();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(file_task, "iris_files",
                    CONFIG_ESP_IRIS_FILE_TASK_STACK_SIZE, NULL,
                    CONFIG_ESP_IRIS_FILE_TASK_PRIORITY,
                    &s_file_task) != pdPASS) {
        iris_files_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void iris_files_deinit(void)
{
    if (s_file_task != NULL && s_work_queue != NULL) {
        xQueueReset(s_work_queue);
        iris_file_work_t work = {.kind = IRIS_FILE_WORK_STOP};
        (void)xQueueSend(s_work_queue, &work, 0);
        for (size_t i = 0; i < 200 && s_file_task != NULL; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_file_task != NULL) {
            vTaskDelete(s_file_task);
            s_file_task = NULL;
        }
    }
    if (s_work_queue != NULL) {
        vQueueDelete(s_work_queue);
        s_work_queue = NULL;
    }
    if (s_completion_queue != NULL) {
        vQueueDelete(s_completion_queue);
        s_completion_queue = NULL;
    }
}

void iris_files_session_end(uint32_t session_id)
{
    if (s_work_queue == NULL || session_id == 0) {
        return;
    }
    xQueueReset(s_work_queue);
    if (s_completion_queue != NULL) {
        xQueueReset(s_completion_queue);
    }
    atomic_store(&s_active_token, 0);
    iris_file_work_t work = {
        .kind = IRIS_FILE_WORK_SESSION_END,
        .session_id = session_id,
    };
    (void)xQueueOverwrite(s_work_queue, &work);
}

static void immediate_status(iris_runtime_t *runtime,
                             const iris_decoded_frame_t *frame,
                             esp_iris_file_status_t status)
{
    uint8_t payload[4] = {0};
    iris_put_le16(payload, (uint16_t)status);
    (void)iris_queue_frame(runtime, ESP_IRIS_CHANNEL_FILE,
                           response_type_for(frame->header.type),
                           ESP_IRIS_FLAG_RESPONSE |
                               (status == ESP_IRIS_FILE_STATUS_OK
                                    ? 0 : ESP_IRIS_FLAG_ERROR),
                           frame->header.request_id, frame->header.stream_id,
                           payload, sizeof(payload));
}

bool iris_files_handle_frame(iris_runtime_t *runtime,
                             const iris_decoded_frame_t *frame)
{
    if (frame->header.channel != ESP_IRIS_CHANNEL_FILE) {
        return false;
    }
    if (s_work_queue == NULL || s_completion_queue == NULL) {
        immediate_status(runtime, frame, ESP_IRIS_FILE_STATUS_NOT_SUPPORTED);
        return true;
    }
    if (frame->header.payload_size > IRIS_FILE_WORK_PAYLOAD_SIZE) {
        immediate_status(runtime, frame, ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);
        return true;
    }
    uint32_t token = ++s_next_token;
    if (token == 0) {
        token = ++s_next_token;
    }
    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&s_active_token, &expected, token)) {
        immediate_status(runtime, frame, ESP_IRIS_FILE_STATUS_BUSY);
        return true;
    }
    iris_file_work_t work = {
        .kind = IRIS_FILE_WORK_FRAME,
        .token = token,
        .type = frame->header.type,
        .session_id = runtime->session_id,
        .request_id = frame->header.request_id,
        .stream_id = frame->header.stream_id,
        .payload_size = (uint16_t)frame->header.payload_size,
    };
    if (frame->header.payload_size > 0) {
        memcpy(work.payload, frame->payload, frame->header.payload_size);
    }
    if (xQueueSend(s_work_queue, &work, 0) != pdTRUE) {
        expected = token;
        (void)atomic_compare_exchange_strong(&s_active_token, &expected, 0);
        immediate_status(runtime, frame, ESP_IRIS_FILE_STATUS_BUSY);
    }
    return true;
}

bool iris_files_queue_next(iris_runtime_t *runtime)
{
    if (s_completion_queue == NULL) {
        return false;
    }
    iris_file_completion_t completion;
    while (xQueueReceive(s_completion_queue, &completion, 0) == pdTRUE) {
        unsigned int expected = completion.token;
        (void)atomic_compare_exchange_strong(&s_active_token, &expected, 0);
        if (runtime->session_id != completion.session_id ||
            !runtime->hello_acked) {
            continue;
        }
        if (iris_get_le16(completion.payload) != ESP_IRIS_FILE_STATUS_OK) {
            completion.flags |= ESP_IRIS_FLAG_ERROR;
        }
        return iris_queue_frame(runtime, ESP_IRIS_CHANNEL_FILE,
                                completion.type, completion.flags,
                                completion.request_id, completion.stream_id,
                                completion.payload,
                                completion.payload_size) == ESP_OK;
    }
    return false;
}

uint32_t iris_files_allocated_bytes(void)
{
    return 0;
}

uint32_t iris_files_static_bytes(void)
{
    return sizeof(s_volumes) + sizeof(s_volume_lock) + sizeof(s_work_queue) +
        sizeof(s_completion_queue) + sizeof(s_file_task) +
        sizeof(s_active_token) + sizeof(s_next_token) + sizeof(s_task_work) +
        sizeof(s_task_completion) + sizeof(s_task_handle) +
        sizeof(s_write_receipt);
}
