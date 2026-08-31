#pragma once

#include "esp_err.h"

void iris_example_provision_pairing(void);
void iris_example_start(void);

/* Call only from a product-owned secure provisioning surface. The caller must
 * deliver the returned token out-of-band and wipe the buffer afterward. */
esp_err_t iris_example_provisioning_rotate_token(char out[65]);
