#include <assert.h>
#include "esp_iris.h"

void app_main(void)
{
    assert(esp_iris_start() == ESP_ERR_NOT_SUPPORTED);
    assert(!esp_iris_is_started());
}
