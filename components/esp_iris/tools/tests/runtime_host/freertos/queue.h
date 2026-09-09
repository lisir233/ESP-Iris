#pragma once

#include <stddef.h>

#include "freertos/FreeRTOS.h"

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(unsigned length, size_t item_size);
int xQueueReset(QueueHandle_t queue);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
int xQueueOverwrite(QueueHandle_t queue, const void *item);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
void vQueueDelete(QueueHandle_t queue);
