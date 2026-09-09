#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/esp_iris_files.c"

typedef struct {
    size_t size;
} allocation_header_t;

typedef struct {
    size_t item_size;
    bool occupied;
    uint8_t item[];
} fake_queue_t;

static size_t free_internal = 1024U * 1024U;
static size_t task_allocation;
static bool fail_task_create;
static unsigned queued_responses;
static uint16_t last_response_status;

iris_runtime_t g_iris;

uint32_t esp_random(void)
{
    static uint32_t value = 100;
    return ++value;
}

static void *tracked_allocate(size_t size)
{
    allocation_header_t *header = malloc(sizeof(*header) + size);
    assert(header != NULL);
    assert(free_internal >= size);
    header->size = size;
    free_internal -= size;
    return header + 1;
}

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    void *allocation = tracked_allocate(count * size);
    memset(allocation, 0, count * size);
    return allocation;
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return tracked_allocate(size);
}

void heap_caps_free(void *allocation)
{
    if (allocation == NULL) {
        return;
    }
    allocation_header_t *header = (allocation_header_t *)allocation - 1;
    free_internal += header->size;
    free(header);
}

size_t heap_caps_get_free_size(unsigned caps)
{
    (void)caps;
    return free_internal;
}

size_t heap_caps_get_minimum_free_size(unsigned caps)
{
    return heap_caps_get_free_size(caps);
}

QueueHandle_t xQueueCreate(unsigned length, size_t item_size)
{
    assert(length == 1);
    fake_queue_t *queue = tracked_allocate(sizeof(*queue) + item_size);
    queue->item_size = item_size;
    queue->occupied = false;
    return queue;
}

int xQueueReset(QueueHandle_t handle)
{
    ((fake_queue_t *)handle)->occupied = false;
    return pdTRUE;
}

int xQueueSend(QueueHandle_t handle, const void *item, TickType_t ticks)
{
    (void)ticks;
    fake_queue_t *queue = handle;
    if (queue->occupied) {
        return 0;
    }
    memcpy(queue->item, item, queue->item_size);
    queue->occupied = true;
    if (*(const iris_file_work_kind_t *)item == IRIS_FILE_WORK_STOP) {
        free_internal += task_allocation;
        task_allocation = 0;
        s_file_task = NULL;
    }
    return pdTRUE;
}

int xQueueOverwrite(QueueHandle_t handle, const void *item)
{
    fake_queue_t *queue = handle;
    memcpy(queue->item, item, queue->item_size);
    queue->occupied = true;
    return pdTRUE;
}

int xQueueReceive(QueueHandle_t handle, void *item, TickType_t ticks)
{
    (void)ticks;
    fake_queue_t *queue = handle;
    if (!queue->occupied) {
        return 0;
    }
    memcpy(item, queue->item, queue->item_size);
    queue->occupied = false;
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t handle)
{
    heap_caps_free(handle);
}

int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack_size,
                void *argument, unsigned priority, TaskHandle_t *task)
{
    (void)entry;
    (void)name;
    (void)argument;
    (void)priority;
    if (fail_task_create) {
        *task = NULL;
        return 0;
    }
    assert(free_internal >= stack_size);
    free_internal -= stack_size;
    task_allocation = stack_size;
    *task = (TaskHandle_t)(uintptr_t)1;
    return pdPASS;
}

void vTaskDelete(void *task)
{
    (void)task;
    free_internal += task_allocation;
    task_allocation = 0;
}

void vTaskDelay(unsigned ticks)
{
    (void)ticks;
}

void xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
}

esp_err_t iris_queue_frame(iris_runtime_t *runtime, uint8_t channel,
                           uint8_t type, uint16_t flags,
                           uint32_t request_id, uint32_t stream_id,
                           const uint8_t *payload, size_t payload_size)
{
    (void)runtime;
    (void)channel;
    (void)type;
    (void)flags;
    (void)request_id;
    (void)stream_id;
    assert(payload_size >= 2);
    last_response_status = iris_get_le16(payload);
    ++queued_responses;
    return ESP_OK;
}

static void assert_released(size_t baseline)
{
    assert(s_task_context == NULL);
    assert(s_work_queue == NULL);
    assert(s_completion_queue == NULL);
    assert(s_file_task == NULL);
    assert(iris_files_allocated_bytes() == 0);
    assert(free_internal == baseline);
}

int main(void)
{
    const size_t baseline = free_internal;
    iris_runtime_t runtime = {.session_id = 42};
    uint8_t payload[IRIS_FILE_WORK_PAYLOAD_SIZE + 1U] = {0};
    iris_decoded_frame_t request = {
        .header = {
            .channel = ESP_IRIS_CHANNEL_FILE,
            .type = ESP_IRIS_FILE_LIST_OPEN,
            .request_id = 7,
            .payload_size = sizeof(payload),
        },
        .payload = payload,
    };

    s_volumes[0].used = true;
    assert_released(baseline);

    assert(iris_files_handle_frame(&runtime, &request));
    assert(s_task_context != NULL);
    assert(s_work_queue != NULL);
    assert(s_completion_queue != NULL);
    assert(s_file_task != NULL);
    assert(iris_files_allocated_bytes() == baseline - free_internal);
    assert(iris_files_allocated_bytes() >= sizeof(*s_task_context));
    assert(queued_responses == 1);
    assert(last_response_status == ESP_IRIS_FILE_STATUS_INVALID_ARGUMENT);

    iris_files_deinit();
    assert_released(baseline);

    fail_task_create = true;
    assert(iris_files_handle_frame(&runtime, &request));
    assert_released(baseline);
    assert(queued_responses == 2);
    assert(last_response_status == ESP_IRIS_FILE_STATUS_NO_MEMORY);

    fail_task_create = false;
    assert(iris_files_handle_frame(&runtime, &request));
    assert(iris_files_allocated_bytes() > 0);
    iris_files_deinit();
    assert_released(baseline);
    return 0;
}
