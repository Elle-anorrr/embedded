/*
 * common.c
 * Defines the shared RTOS objects that cocis_common.h declares with `extern`.
 * `extern` in the header = "this exists somewhere".
 * The lines below are "here it is" (one definition, in one .c file only).
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/i2c.h"
#include "cocis_common.h"

static const char *TAG = "common";

#define MINUTES_PER_DAY       1440
#define MIN_VALID_CLOCK_YEAR  2020
#define DEFAULT_SCHEDULE_START 420   /* 07:00 */
#define DEFAULT_SCHEDULE_END   1140  /* 19:00 */

#define DS1307_I2C_PORT         I2C_NUM_0
#define DS1307_I2C_SCL_PIN      PIN_RTC_SCL
#define DS1307_I2C_SDA_PIN      PIN_RTC_SDA
#define DS1307_ADDRESS          0x68
#define DS1307_REG_SEC          0x00
#define DS1307_REG_MIN          0x01
#define DS1307_REG_HOUR         0x02
#define DS1307_REG_DAY          0x03
#define DS1307_REG_DATE         0x04
#define DS1307_REG_MONTH        0x05
#define DS1307_REG_YEAR         0x06
#define DS1307_REG_CONTROL      0x07
#define NVS_NAMESPACE           "cocis_cfg"
#define NVS_KEY_START           "sched_start"
#define NVS_KEY_END             "sched_end"

QueueHandle_t     scan_queue = NULL;
QueueHandle_t     log_queue  = NULL;
SemaphoreHandle_t occ_mutex  = NULL;
SemaphoreHandle_t cfg_mutex  = NULL;
SemaphoreHandle_t room_mutex = NULL;

static schedule_t s_schedule = {
    .start_min = DEFAULT_SCHEDULE_START,
    .end_min = DEFAULT_SCHEDULE_END,
    .enabled = true,
};

static uint8_t bcd_to_u8(uint8_t value)
{
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

static uint8_t u8_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static bool schedule_load_nvs(void)
{
    esp_err_t err;
    uint16_t start = DEFAULT_SCHEDULE_START;
    uint16_t end = DEFAULT_SCHEDULE_END;
    nvs_handle_t handle = 0;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_get_u16(handle, NVS_KEY_START, &start);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        start = DEFAULT_SCHEDULE_START;
        err = nvs_set_u16(handle, NVS_KEY_START, start);
    }
    if (err == ESP_OK) {
        err = nvs_get_u16(handle, NVS_KEY_END, &end);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        end = DEFAULT_SCHEDULE_END;
        err = nvs_set_u16(handle, NVS_KEY_END, end);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS schedule load failed: %s", esp_err_to_name(err));
        return false;
    }

    if (start >= MINUTES_PER_DAY || end >= MINUTES_PER_DAY || start == end) {
        start = DEFAULT_SCHEDULE_START;
        end = DEFAULT_SCHEDULE_END;
    }

    s_schedule.start_min = start;
    s_schedule.end_min = end;
    s_schedule.enabled = true;
    return true;
}

static bool schedule_store_nvs_locked(void)
{
    esp_err_t err;
    nvs_handle_t handle = 0;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for save failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_u16(handle, NVS_KEY_START, s_schedule.start_min);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, NVS_KEY_END, s_schedule.end_min);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ds1307_rtc_init(void)
{
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DS1307_I2C_SDA_PIN,
        .scl_io_num = DS1307_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t err = i2c_param_config(DS1307_I2C_PORT, &i2c_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC I2C config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2c_driver_install(DS1307_I2C_PORT, i2c_cfg.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC I2C install failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t reg = DS1307_REG_SEC;
    uint8_t sec = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS1307_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS1307_ADDRESS << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &sec, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(DS1307_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC read failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

bool rtc_get_time(time_t *out)
{
    if (out == NULL) {
        return false;
    }

    uint8_t regs[7] = {0};
    uint8_t reg = DS1307_REG_SEC;
    esp_err_t err = i2c_master_write_read_device(DS1307_I2C_PORT,
                                               DS1307_ADDRESS,
                                               &reg,
                                               1,
                                               regs,
                                               sizeof(regs),
                                               pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC read time failed: %s", esp_err_to_name(err));
        return false;
    }

    struct tm tm_val = {0};
    tm_val.tm_sec = bcd_to_u8(regs[0] & 0x7F);
    tm_val.tm_min = bcd_to_u8(regs[1]);
    tm_val.tm_hour = bcd_to_u8(regs[2] & 0x3F);
    tm_val.tm_mday = bcd_to_u8(regs[3]);
    tm_val.tm_mon = bcd_to_u8(regs[4] & 0x1F) - 1;
    tm_val.tm_year = bcd_to_u8(regs[6]) + 100;
    tm_val.tm_isdst = -1;

    *out = mktime(&tm_val);
    if (*out == (time_t)-1) {
        return false;
    }
    return true;
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

bool access_is_allowed(bool is_entry, bool currently_inside, time_t now)
{
    if (!is_entry) {
        return currently_inside;
    }
    return within_schedule(now);
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
    bool ok = schedule_store_nvs_locked();
    xSemaphoreGive(cfg_mutex);
    return ok;
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

void common_init(void)
{
    scan_queue = xQueueCreate(10, sizeof(scan_t));
    log_queue  = xQueueCreate(20, sizeof(log_event_t));
    occ_mutex  = xSemaphoreCreateMutex();
    cfg_mutex  = xSemaphoreCreateMutex();
    room_mutex = xSemaphoreCreateMutex();

    configASSERT(scan_queue && log_queue && occ_mutex && cfg_mutex && room_mutex);

    if (!ds1307_rtc_init()) {
        ESP_LOGW(TAG, "RTC init failed; schedule checks need a valid RTC clock");
    }

    if (!schedule_load_nvs()) {
        s_schedule.start_min = DEFAULT_SCHEDULE_START;
        s_schedule.end_min = DEFAULT_SCHEDULE_END;
        s_schedule.enabled = true;
        ESP_LOGW(TAG, "using default schedule 07:00-19:00");
    }

    ESP_LOGI(TAG, "shared queues and mutexes created");
}
