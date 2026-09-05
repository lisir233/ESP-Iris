/* Included by services.c to keep private service ownership in one unit.
 * Only the protocol task submits/drains; only iris-service executes work.
 * Atomic stage release/acquire publishes the owned input and completion. */
enum { IRIS_EXEC_IDLE, IRIS_EXEC_QUEUED, IRIS_EXEC_RUNNING, IRIS_EXEC_DONE, IRIS_EXEC_RESERVED, IRIS_EXEC_DRAINING };
static struct {
    atomic_int stage;
    atomic_bool abandoned;
    atomic_bool deinit;
    atomic_bool ota_cancel;
    TaskHandle_t task;
    iris_runtime_t *work;
    esp_iris_wire_header_t header;
    iris_decoded_frame_t completion;
    bool completion_decoded;
    uint64_t received_us;
    esp_iris_ota_status_t ota_status;
    uint8_t ota_snapshot[25];
    size_t ota_snapshot_size;
} s_executor;

static bool executor_busy(void)
{
    return atomic_load(&s_executor.stage) != IRIS_EXEC_IDLE;
}

bool iris_services_work_pending(void)
{
    return executor_busy();
}

static bool executor_cancelled(void)
{
    return atomic_load(&s_executor.abandoned) || atomic_load(&s_executor.ota_cancel);
}

static void executor_request_ota_cancel(void)
{
    atomic_store(&s_executor.ota_cancel, true);
}

static uint32_t executor_allocated_bytes(void)
{
    return s_executor.work == NULL ? 0 : sizeof(*s_executor.work) +
        CONFIG_ESP_IRIS_SERVICE_STACK_SIZE;
}

static bool executor_ota_status(esp_iris_ota_status_t *out)
{
#if CONFIG_ESP_IRIS_OTA
    if (executor_busy()) {
        taskENTER_CRITICAL(&s_services_lock);
        *out = s_executor.ota_status;
        taskEXIT_CRITICAL(&s_services_lock);
        return true;
    }
#else
    (void)out;
#endif
    return false;
}

static void executor_snapshot(void)
{
#if CONFIG_ESP_IRIS_OTA
    uint8_t payload[25] = {0};
    esp_iris_ota_status_t status = {0};
    iris_service_state_t *state = service_state(false);
    size_t label_size = 0;
    if (valid_state(state)) {
        iris_ota_t *ota = &state->ota;
        esp_iris_job_info_t info = {0};
        if (ota->job != NULL) {
            (void)esp_iris_job_get_info(ota->job, &info);
        }
        status = (esp_iris_ota_status_t) {
            .job_id = info.id, .total_size = ota->total_size,
            .received_size = ota->received, .progress_permille = info.progress_permille,
            .state = info.state, .result = info.result, .active = ota->active,
        };
        iris_put_le32(payload, info.id);
        iris_put_le32(payload + 4, ota->total_size);
        iris_put_le32(payload + 8, ota->received);
        iris_put_le16(payload + 12, info.progress_permille);
        payload[14] = ota->active ? 1 : 0;
        label_size = ota->partition == NULL ? 0 : strnlen(ota->partition->label, 5);
        payload[15] = (uint8_t)label_size;
        iris_put_le32(payload + 16, (uint32_t)info.result);
        if (label_size != 0) memcpy(payload + 20, ota->partition->label, label_size);
    }
    taskENTER_CRITICAL(&s_services_lock);
    s_executor.ota_status = status;
    memcpy(s_executor.ota_snapshot, payload, sizeof(payload));
    s_executor.ota_snapshot_size = 20 + label_size;
    taskEXIT_CRITICAL(&s_services_lock);
#endif
}

static void executor_run_one(void)
{
    int queued = IRIS_EXEC_QUEUED;
    if (!atomic_compare_exchange_strong(&s_executor.stage, &queued, IRIS_EXEC_RUNNING)) {
        return;
    }
    iris_runtime_t *work = s_executor.work;
    iris_decoded_frame_t frame = {.header = s_executor.header, .payload = work->rx_wire};
    if (!atomic_load(&s_executor.abandoned)) {
        if (frame.header.channel == ESP_IRIS_CHANNEL_CONTROL) {
            if (!handle_rpc(work, &frame, s_executor.received_us)) {
                (void)handle_job(work, &frame);
            }
        } else if (frame.header.channel == ESP_IRIS_CHANNEL_SYSTEM_UPDATE) {
            /* Inventory shares the channel, but has a separate read-only provider. */
            if (!iris_system_inventory_handle_frame(work, &frame) &&
                !iris_system_update_handle_frame(work, &frame)) {
                (void)iris_queue_error(work, frame.header.request_id,
                    ESP_ERR_NOT_SUPPORTED, frame.header.channel, frame.header.type);
            }
        } else {
            (void)handle_ota(work, &frame);
        }
    }
    executor_snapshot();
    bool cleaned = false;
    for (;;) {
        taskENTER_CRITICAL(&s_services_lock);
        const bool abandoned = atomic_exchange(&s_executor.abandoned, false);
        const bool deinit = abandoned && atomic_exchange(&s_executor.deinit, false);
        const bool ota_cancel = atomic_exchange(&s_executor.ota_cancel, false);
        iris_service_state_t *state = s_services;
        esp_iris_job_cancel_fn cancel = NULL;
        void *ctx = NULL;
        bool cancel_pending = false;
        if (valid_state(state)) {
            for (size_t i = 0; i < CONFIG_ESP_IRIS_MAX_JOBS; ++i) {
                if (state->jobs[i].cancel_pending) {
                    state->jobs[i].cancel_pending = false;
                    cancel_pending = true;
                    cancel = state->jobs[i].cancel;
                    ctx = state->jobs[i].user_ctx;
                    break;
                }
            }
        }
        const bool system_cancel = iris_system_update_cancel_pending();
        if (!abandoned && !ota_cancel && !cancel_pending && !system_cancel) {
            atomic_store(&s_executor.stage, cleaned ? IRIS_EXEC_IDLE : IRIS_EXEC_DONE);
            taskEXIT_CRITICAL(&s_services_lock);
            break;
        }
        taskEXIT_CRITICAL(&s_services_lock);
        if (cancel != NULL) cancel(ctx);
        if (system_cancel) iris_system_update_poll_cancel();
        if (ota_cancel && valid_state(state)) ota_abort(state, ESP_ERR_INVALID_STATE);
        if (abandoned) {
            services_session_end_now(work);
            if (deinit) services_deinit_now(work);
            state = service_state(false);
            if (valid_state(state)) {
                state->rpc_request_seen = false;
                state->last_rpc_request_id = 0;
            }
            work->tx_wire_length = 0;
            cleaned = true;
        }
        executor_snapshot();
    }
    iris_notify_worker(&g_iris);
}

static void executor_worker(void *argument)
{
    (void)argument;
    /* Retained across stop/start: exactly one lazy, bounded worker per device. */
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        executor_run_one();
    }
}

static esp_err_t executor_ensure(void)
{
    if (s_executor.work == NULL) {
        s_executor.work = heap_caps_calloc(1, sizeof(*s_executor.work), MALLOC_CAP_INTERNAL);
        if (s_executor.work == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_executor.task == NULL &&
        xTaskCreate(executor_worker, "iris-service", CONFIG_ESP_IRIS_SERVICE_STACK_SIZE,
                    NULL, CONFIG_ESP_IRIS_TASK_PRIORITY, &s_executor.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool executor_abandon(iris_runtime_t *runtime, bool deinit)
{
    if (s_executor.work == NULL || s_executor.task == NULL) return false;
    taskENTER_CRITICAL(&s_services_lock);
    atomic_store(&s_executor.deinit, deinit || atomic_load(&s_executor.deinit));
    atomic_store(&s_executor.abandoned, true);
    executor_request_ota_cancel();
    iris_system_update_cancel_all();
    const int stage = atomic_load(&s_executor.stage);
    if (stage == IRIS_EXEC_IDLE || stage == IRIS_EXEC_DONE) {
        s_executor.work->session_id = runtime->session_id;
        atomic_store(&s_executor.stage, IRIS_EXEC_QUEUED);
    }
    taskEXIT_CRITICAL(&s_services_lock);
    xTaskNotifyGive(s_executor.task);
    return true;
}

void iris_services_deinit(iris_runtime_t *runtime)
{
    if (!executor_abandon(runtime, true)) services_deinit_now(runtime);
}

void iris_services_session_end(iris_runtime_t *runtime)
{
    if (!executor_abandon(runtime, false)) services_session_end_now(runtime);
}

static bool executor_dispatch(iris_runtime_t *runtime,
                              const iris_decoded_frame_t *frame,
                              uint64_t received_us)
{
    const esp_iris_wire_header_t *h = &frame->header;
    const bool rpc = h->channel == ESP_IRIS_CHANNEL_CONTROL && h->type == ESP_IRIS_CONTROL_REQUEST;
    const bool cancel = h->channel == ESP_IRIS_CHANNEL_CONTROL && h->type == ESP_IRIS_CONTROL_CANCEL;
    const bool ota = h->channel == ESP_IRIS_CHANNEL_OTA;
    const bool system_update = h->channel == ESP_IRIS_CHANNEL_SYSTEM_UPDATE;
    const bool busy = executor_busy();
    if (system_update && h->type == ESP_IRIS_SYSTEM_UPDATE_STATUS) {
        return iris_system_update_handle_frame(runtime, frame);
    }
    if (busy) {
        if (system_update && h->type == ESP_IRIS_SYSTEM_UPDATE_CANCEL) {
            taskENTER_CRITICAL(&s_services_lock);
            const esp_err_t err = atomic_load(&s_executor.stage) == IRIS_EXEC_DONE
                ? ESP_ERR_INVALID_STATE
                : iris_system_update_request_cancel(frame->payload, h->payload_size);
            taskEXIT_CRITICAL(&s_services_lock);
            if (err != ESP_OK) {
                (void)iris_queue_error(runtime, h->request_id, err, h->channel, h->type);
            } else {
                iris_decoded_frame_t status = *frame;
                status.header.type = ESP_IRIS_SYSTEM_UPDATE_STATUS;
                status.header.payload_size = 0;
                (void)iris_system_update_handle_frame(runtime, &status);
            }
            return true;
        }
#if CONFIG_ESP_IRIS_OTA
        if (ota && h->type == ESP_IRIS_OTA_STATUS && h->payload_size == 0) {
            uint8_t snapshot[25]; size_t size;
            taskENTER_CRITICAL(&s_services_lock);
            size = s_executor.ota_snapshot_size;
            memcpy(snapshot, s_executor.ota_snapshot, sizeof(snapshot));
            taskEXIT_CRITICAL(&s_services_lock);
            (void)iris_queue_frame(runtime, h->channel, h->type, ESP_IRIS_FLAG_RESPONSE,
                h->request_id, iris_get_le32(snapshot), snapshot, size);
            return true;
        }
        if (ota && h->type == ESP_IRIS_OTA_CANCEL && h->payload_size == 0) {
            taskENTER_CRITICAL(&s_services_lock);
            const bool accepted = atomic_load(&s_executor.stage) != IRIS_EXEC_DONE;
            if (accepted) executor_request_ota_cancel();
            taskEXIT_CRITICAL(&s_services_lock);
            if (!accepted) {
                (void)iris_queue_error(runtime, h->request_id, ESP_ERR_INVALID_STATE, h->channel, h->type);
                return true;
            }
            /* Acknowledges intent only: no STREAM_END until the worker stops. */
            (void)iris_queue_frame(runtime, h->channel, ESP_IRIS_OTA_STATUS,
                ESP_IRIS_FLAG_RESPONSE, h->request_id, h->stream_id, NULL, 0);
            return true;
        }
#endif
        if (cancel && h->payload_size == 4) {
            iris_service_state_t *state = service_state(false);
            uint8_t payload[16]; bool found = false;
            taskENTER_CRITICAL(&s_services_lock);
            struct esp_iris_job *job = find_job(state, iris_get_le32(frame->payload));
            if (job != NULL && atomic_load(&s_executor.stage) != IRIS_EXEC_DONE) {
                found = true;
                if (job->info.state == ESP_IRIS_JOB_RUNNING) {
                    job->info.cancel_requested = true;
                    job->cancel_pending = true;
                    job->event_pending = true;
                }
                job_encode(payload, &job->info);
            }
            taskEXIT_CRITICAL(&s_services_lock);
            if (found) {
                (void)iris_queue_frame(runtime, h->channel, ESP_IRIS_CONTROL_JOB_STATUS,
                    ESP_IRIS_FLAG_RESPONSE, h->request_id, 0, payload, sizeof(payload));
            } else {
                (void)iris_queue_error(runtime, h->request_id, ESP_ERR_NOT_FOUND, h->channel, h->type);
            }
            return true;
        }
        if (h->channel == ESP_IRIS_CHANNEL_CONTROL && h->type == ESP_IRIS_CONTROL_JOB_QUERY) {
            return handle_job(runtime, frame);
        }
        (void)iris_queue_error(runtime, h->request_id, ESP_ERR_INVALID_STATE, h->channel, h->type);
        return true;
    }
    if (!rpc && !cancel && !ota && !system_update) return false;
    taskENTER_CRITICAL(&s_services_lock);
    const bool idle = atomic_load(&s_executor.stage) == IRIS_EXEC_IDLE;
    if (idle) {
        atomic_store(&s_executor.abandoned, false);
        atomic_store(&s_executor.deinit, false);
        atomic_store(&s_executor.ota_cancel, false);
        atomic_store(&s_executor.stage, IRIS_EXEC_RESERVED);
    }
    taskEXIT_CRITICAL(&s_services_lock);
    if (!idle) {
        (void)iris_queue_error(runtime, h->request_id, ESP_ERR_INVALID_STATE, h->channel, h->type);
        return true;
    }
    const esp_err_t err = executor_ensure();
    if (err != ESP_OK) {
        atomic_store(&s_executor.stage, IRIS_EXEC_IDLE);
        (void)iris_queue_error(runtime, h->request_id, err, h->channel, h->type);
        return true;
    }
    executor_snapshot();
    s_executor.header = *h;
    s_executor.completion_decoded = false;
    s_executor.received_us = received_us;
    s_executor.work->session_id = runtime->session_id;
    s_executor.work->tx_wire_length = 0;
    s_executor.work->tx_wire_offset = 0;
    memcpy(s_executor.work->rx_wire, frame->payload, h->payload_size);
    atomic_store(&s_executor.stage, IRIS_EXEC_QUEUED);
    xTaskNotifyGive(s_executor.task);
    return true;
}

static bool executor_queue_next(iris_runtime_t *runtime)
{
    if (runtime->tx_wire_length != 0) return false;
    taskENTER_CRITICAL(&s_services_lock);
    const bool ready = atomic_load(&s_executor.stage) == IRIS_EXEC_DONE;
    if (ready) atomic_store(&s_executor.stage, IRIS_EXEC_DRAINING);
    taskEXIT_CRITICAL(&s_services_lock);
    if (!ready) return false;
    iris_runtime_t *work = s_executor.work;
    bool queued = false;
    bool retry = false;
    if (work->session_id == runtime->session_id && work->tx_wire_length != 0 &&
        !atomic_load(&s_executor.abandoned)) {
        if (!s_executor.completion_decoded) {
            s_executor.completion_decoded = iris_frame_decode_in_place(
                work->tx_wire, work->tx_wire_length - 1, &s_executor.completion) == ESP_OK;
        }
        if (s_executor.completion_decoded) {
            esp_iris_wire_header_t *h = &s_executor.completion.header;
            queued = iris_queue_frame(runtime, h->channel, h->type, h->flags,
                h->request_id, h->stream_id, s_executor.completion.payload, h->payload_size) == ESP_OK;
            retry = !queued;
        } else {
            ++runtime->invalid_frames;
        }
    }
    taskENTER_CRITICAL(&s_services_lock);
    const bool cleanup = atomic_load(&s_executor.abandoned);
    if (!retry) work->tx_wire_length = 0;
    atomic_store(&s_executor.stage, cleanup ? IRIS_EXEC_QUEUED :
                 (retry ? IRIS_EXEC_DONE : IRIS_EXEC_IDLE));
    taskEXIT_CRITICAL(&s_services_lock);
    if (cleanup) xTaskNotifyGive(s_executor.task);
    return queued;
}
