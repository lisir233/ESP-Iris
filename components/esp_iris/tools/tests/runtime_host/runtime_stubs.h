/* Hardware and unrelated providers only; runtime, services, codec and
 * transport arbitration below run their production implementations. */
unsigned uxTaskGetStackHighWaterMark2(void *p) { return 9999; }
unsigned ulTaskNotifyTake(int b, unsigned n) { return 0; }
void vTaskDelete(void *p) { }
int xTaskCreate(void (*f)(void *),const char *n,unsigned s,void *p,unsigned pri,TaskHandle_t *t) { return 0; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
void xTaskNotifyGive(TaskHandle_t t) { }
unsigned xTaskGetTickCount(void) { return 0; }
void vTaskDelay(unsigned n) { }
const esp_app_desc_t *esp_app_get_description(void) { static esp_app_desc_t d; return &d; }
size_t heap_caps_get_free_size(unsigned c) { return 10000; }
size_t heap_caps_get_minimum_free_size(unsigned c) { return 10000; }
int esp_reset_reason(void) { return 0; }
void esp_restart(void) { assert(0); }
esp_err_t iris_identity_load_or_create(iris_runtime_t *r) { return ESP_OK; }
esp_err_t iris_log_vfs_init(iris_runtime_t *r) { return ESP_OK; }
esp_err_t iris_log_vfs_deinit(void) { return ESP_OK; }
esp_err_t iris_log_redirect_stdio(void) { return ESP_OK; }
esp_err_t iris_log_restore_stdio(void) { return ESP_OK; }
bool iris_log_pop(iris_runtime_t *r, size_t n, iris_log_record_t *o) { return false; }
void iris_crash_probe(iris_runtime_t *r) { }
esp_err_t iris_crash_build_metadata(iris_runtime_t *r, uint8_t *o, size_t n, size_t *s) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t iris_crash_read(iris_runtime_t *r, size_t a, size_t n, uint8_t *o, size_t *s) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t iris_files_init(iris_runtime_t *r) { return ESP_OK; }
void iris_files_deinit(void) { }
void iris_files_session_end(uint32_t s) { }
void iris_system_update_session_end(void) { }
bool iris_files_handle_frame(iris_runtime_t *r, const iris_decoded_frame_t *f) { return false; }
bool iris_system_inventory_handle_frame(iris_runtime_t *r, const iris_decoded_frame_t *f) { return false; }
bool iris_system_update_handle_frame(iris_runtime_t *r, const iris_decoded_frame_t *f) { return false; }
bool iris_files_queue_next(iris_runtime_t *r) { return false; }
uint64_t iris_files_capabilities(void) { return 0; }
uint64_t iris_system_inventory_capabilities(void) { return 0; }
uint64_t iris_system_update_capabilities(void) { return 0; }
uint32_t iris_files_allocated_bytes(void) { return 0; }
uint32_t iris_files_static_bytes(void) { return 0; }
uint32_t iris_system_inventory_static_bytes(void) { return 0; }
uint32_t iris_system_update_static_bytes(void) { return 0; }
esp_err_t esp_iris_platform_mark_planned_restart(void) { return ESP_OK; }
esp_err_t esp_iris_platform_mark_healthy(void) { return ESP_OK; }

static uint8_t incoming[256], outgoing[1024];
static size_t incoming_size, incoming_offset, read_limit = 256;
static size_t outgoing_size, write_limit = 1024;
static bool candidate;
static unsigned starts, stops;
static esp_err_t fake_start(iris_runtime_t *r, iris_transport_state_t *s) { ++starts; s->driver_started = true; return ESP_OK; }
static void fake_stop(iris_runtime_t *r, iris_transport_state_t *s) { ++stops; s->driver_started = false; }
static iris_link_event_t fake_poll(iris_runtime_t *r, iris_transport_state_t *s) { if (candidate) { candidate = false; return IRIS_LINK_EVENT_CONNECTED; } return IRIS_LINK_EVENT_NONE; }
static void fake_disconnect(iris_runtime_t *r, iris_transport_state_t *s) { }
static int fake_read(iris_runtime_t *r, iris_transport_state_t *s, uint8_t *b, size_t n) {
    if (n > incoming_size - incoming_offset) n = incoming_size - incoming_offset;
    if (n > read_limit) n = read_limit;
    memcpy(b, incoming + incoming_offset, n); incoming_offset += n; return (int)n;
}
static int fake_write(iris_runtime_t *r, iris_transport_state_t *s, const uint8_t *b, size_t n) {
    if (n > write_limit) n = write_limit;
    assert(outgoing_size + n <= sizeof(outgoing));
    memcpy(outgoing + outgoing_size, b, n); outgoing_size += n; return (int)n;
}
const iris_transport_ops_t g_iris_tcp_transport_ops = {
    ESP_IRIS_TRANSPORT_KIND_TCP, "tcp", fake_start, fake_stop,
    fake_poll, fake_disconnect, fake_read, fake_write };
#if CONFIG_ESP_IRIS_TRANSPORT_USB
const iris_transport_ops_t g_iris_usb_transport_ops = {
    ESP_IRIS_TRANSPORT_KIND_USB, "usb", fake_start, fake_stop,
    fake_poll, fake_disconnect, fake_read, fake_write };
#endif
