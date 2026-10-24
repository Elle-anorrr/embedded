/*
 * common.c
 * Defines the shared RTOS objects that cocis_common.h declares with `extern`.
 * `extern` in the header = "this exists somewhere".
 * The lines below are "here it is" (one definition, in one .c file only).
 */
#include "esp_log.h"
#include "cocis_common.h"

static const char *TAG = "common";

QueueHandle_t     scan_queue = NULL;
QueueHandle_t     log_queue  = NULL;
SemaphoreHandle_t occ_mutex  = NULL;
SemaphoreHandle_t cfg_mutex  = NULL;
SemaphoreHandle_t room_mutex = NULL;

void common_init(void)
{
    scan_queue = xQueueCreate(10, sizeof(scan_t));
    log_queue  = xQueueCreate(20, sizeof(log_event_t));
    occ_mutex  = xSemaphoreCreateMutex();
    cfg_mutex  = xSemaphoreCreateMutex();
    room_mutex = xSemaphoreCreateMutex();

    /* If any allocation failed, stop immediately instead of crashing later. */
    configASSERT(scan_queue && log_queue && occ_mutex && cfg_mutex && room_mutex);
    ESP_LOGI(TAG, "shared queues and mutexes created");
}