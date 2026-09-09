#include "esp_iris_internal.h"

#include <string.h>

#include "esp_mac.h"
#include "esp_random.h"

/* Keep the existing 16-byte Device ID wire shape while making the identity a
 * direct, reversible function of the factory eFuse MAC.  The ten-byte domain
 * prefix prevents these values from being confused with legacy random UUIDs. */
static const uint8_t s_hardware_id_prefix[10] = {
    'E', 'S', 'P', '-', 'I', 'R', 'I', 'S', 1, 0,
};

esp_err_t iris_identity_load_or_create(iris_runtime_t *runtime)
{
    if (runtime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ESP32-S31 supports IEEE 802.15.4, so esp_efuse_mac_get_default()
     * returns an 8-byte EUI-64 and inserts the MAC extension in the middle.
     * ESP-Iris identity intentionally uses the immutable 6-byte MAC_FACTORY
     * value reported by the ROM's `read-mac` command. */
    esp_err_t err = esp_read_mac(runtime->hardware_mac,
                                 ESP_MAC_EFUSE_FACTORY);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(runtime->device_id, s_hardware_id_prefix,
           sizeof(s_hardware_id_prefix));
    memcpy(runtime->device_id + sizeof(s_hardware_id_prefix),
           runtime->hardware_mac, sizeof(runtime->hardware_mac));

    do {
        esp_fill_random(&runtime->boot_id, sizeof(runtime->boot_id));
    } while (runtime->boot_id == 0);
    return ESP_OK;
}
