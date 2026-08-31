#include "esp_iris.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if CONFIG_ESP_IRIS_TRANSPORT_TCP
#include "esp_mac.h"
#include "mdns.h"

#define IRIS_MDNS_SERVICE "_esp-iris"
#define IRIS_MDNS_PROTO "_tcp"
#define IRIS_MDNS_DEFAULT_PREFIX "ESP-Iris"
#define IRIS_MDNS_DEFAULT_MODE "normal"
#define IRIS_MDNS_MODE_MAX 31U

static bool s_mdns_registered;

esp_err_t esp_iris_mdns_register(const esp_iris_mdns_config_t *config)
{
    if (s_mdns_registered) {
        return ESP_OK;
    }

    char hostname[MDNS_NAME_BUF_LEN];
    esp_err_t err = mdns_hostname_get(hostname);
    if (err != ESP_OK || hostname[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (mdns_service_exists(IRIS_MDNS_SERVICE, IRIS_MDNS_PROTO, NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    char device_id[33];
    err = esp_iris_format_device_id(device_id);
    if (err != ESP_OK) {
        return err;
    }

    const char *prefix = config != NULL && config->instance_prefix != NULL
                             ? config->instance_prefix
                             : IRIS_MDNS_DEFAULT_PREFIX;
    const char *mode = config != NULL && config->mode != NULL
                           ? config->mode
                           : IRIS_MDNS_DEFAULT_MODE;
    const size_t prefix_length = strlen(prefix);
    const size_t mode_length = strlen(mode);
    if (prefix_length == 0 || prefix_length + 7U > MDNS_NAME_MAX_LEN ||
            mode_length == 0 || mode_length > IRIS_MDNS_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6];
    err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }
    char instance[MDNS_NAME_BUF_LEN];
    const int instance_length = snprintf(
        instance, sizeof(instance), "%s-%02x%02x%02x", prefix,
        mac[3], mac[4], mac[5]);
    if (instance_length <= 0 || (size_t)instance_length >= sizeof(instance)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char protocol[6];
    char port[6];
    snprintf(protocol, sizeof(protocol), "%u", ESP_IRIS_PROTOCOL_VERSION);
    snprintf(port, sizeof(port), "%u", CONFIG_ESP_IRIS_TCP_PORT);
    mdns_txt_item_t txt[] = {
        {.key = "device_id", .value = device_id},
        {.key = "protocol", .value = protocol},
        {.key = "transport", .value = "tcp"},
#if CONFIG_ESP_IRIS_TCP_PAIRING
        {.key = "pairing", .value = "hmac"},
#else
        {.key = "pairing", .value = "none"},
#endif
        {.key = "mode", .value = mode},
        {.key = "port", .value = port},
    };
    err = mdns_service_add(instance, IRIS_MDNS_SERVICE, IRIS_MDNS_PROTO,
                           CONFIG_ESP_IRIS_TCP_PORT, txt,
                           sizeof(txt) / sizeof(txt[0]));
    if (err == ESP_OK) {
        s_mdns_registered = true;
    }
    return err;
}

esp_err_t esp_iris_mdns_unregister(void)
{
    if (!s_mdns_registered) {
        return ESP_OK;
    }
    esp_err_t err = mdns_service_remove(IRIS_MDNS_SERVICE, IRIS_MDNS_PROTO);
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        s_mdns_registered = false;
        return ESP_OK;
    }
    return err;
}

#else

esp_err_t esp_iris_mdns_register(const esp_iris_mdns_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_iris_mdns_unregister(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
