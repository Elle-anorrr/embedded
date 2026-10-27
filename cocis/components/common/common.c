/*
 * common.c
 * Defines the shared RTOS objects that cocis_common.h declares with `extern`.
 * `extern` in the header = "this exists somewhere".
 * The lines below are "here it is" (one definition, in one .c file only).
 */
#include "esp_log.h"
#include "cocis_common.h"

static const char *TAG = "common";

#define MINUTES_PER_DAY 1440
#define MIN_VALID_CLOCK_YEAR 2020

QueueHandle_t     scan_queue = NULL;
QueueHandle_t     log_queue  = NULL;
SemaphoreHandle_t occ_mutex  = NULL;
SemaphoreHandle_t cfg_mutex  = NULL;
SemaphoreHandle_t room_mutex = NULL;

static schedule_t s_schedule = {
    .start_min = 480,
    .end_min = 1080,
    .enabled = true,
};

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

bool within_schedule(time_t now)
{
    struct tm local_time;
    if (now <= 0 || !localtime_r(&now, &local_time) ||
        local_time.tm_year + 1900 < MIN_VALID_CLOCK_YEAR || cfg_mutex == NULL) {
        return false;
    }

    uint16_t minute = (uint16_t)(local_time.tm_hour * 60 + local_time.tm_min);
    if (xSemaphoreTake(cfg_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    schedule_t schedule = s_schedule;
    xSemaphoreGive(cfg_mutex);

    if (!schedule.enabled) {
        return false;
    }
    if (schedule.start_min < schedule.end_min) {
        return minute >= schedule.start_min && minute < schedule.end_min;
    }
    return minute >= schedule.start_min || minute < schedule.end_min;
}

bool schedule_set(uint16_t start_min, uint16_t end_min)
{
    if (start_min >= MINUTES_PER_DAY || end_min >= MINUTES_PER_DAY ||
        start_min == end_min || cfg_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(cfg_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    s_schedule.start_min = start_min;
    s_schedule.end_min = end_min;
    s_schedule.enabled = true;
    xSemaphoreGive(cfg_mutex);
    return true;
}

bool schedule_get(schedule_t *out)
{
    if (out == NULL || cfg_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(cfg_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    *out = s_schedule;
    xSemaphoreGive(cfg_mutex);
    return true;
}
