#pragma once
typedef void *TaskHandle_t;
unsigned uxTaskGetStackHighWaterMark2(void *);
unsigned ulTaskNotifyTake(int, unsigned);
void vTaskDelete(void *);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void xTaskNotifyGive(TaskHandle_t);
unsigned xTaskGetTickCount(void);
void vTaskDelay(unsigned);

