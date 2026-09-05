#pragma once
#include <stdint.h>
typedef struct { unsigned address; unsigned size; char label[17]; unsigned type; } esp_partition_t;
typedef void *esp_partition_iterator_t;
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_ANY 0xff
esp_partition_iterator_t esp_partition_find(unsigned, unsigned, const char *);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t);
void esp_partition_iterator_release(esp_partition_iterator_t);
