#include "cocis_common.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include <sys/stat.h>

static const char *TAG = "storage";
static void find_next_idx(void);


#define MOUNT_POINT "/sdcard"
#define SD_HOST     SPI2_HOST

static sdmmc_card_t     *s_card;
static SemaphoreHandle_t s_sd_mutex;   /* private: guards the log file */

bool storage_init(void)
{
    s_sd_mutex = xSemaphoreCreateMutex();

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(SD_HOST, &bus, SDSPI_DEFAULT_DMA);
    /* INVALID_STATE means a teammate already initialised the bus: fine */
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SD_HOST;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
   find_next_idx();
    return true;
}
#define LOG_PATH MOUNT_POINT "/log.csv"
#define LOG_MAX_BYTES 2048     /* small so rotation is easy to demo; raise later */

static int  s_next_idx = 1;
static char s_inside[MAX_INSIDE][UID_MAX_LEN];
static int  s_inside_n;

static void rotated_path(char *out, size_t n, int idx)
{
    snprintf(out, n, MOUNT_POINT "/log_%03d.csv", idx);
}

static void find_next_idx(void)
{
    struct stat st; char p[40];
    s_next_idx = 1;
    for (;;) {
        rotated_path(p, sizeof p, s_next_idx);
        if (stat(p, &st) != 0) break;
        s_next_idx++;
    }
}

static void rotate_if_needed(void)      /* caller holds s_sd_mutex */
{
    struct stat st; char p[40];
    if (stat(LOG_PATH, &st) == 0 && st.st_size >= LOG_MAX_BYTES) {
        rotated_path(p, sizeof p, s_next_idx);
        if (rename(LOG_PATH, p) == 0) {
            ESP_LOGI(TAG, "rotated log to %s", p);
            s_next_idx++;
        }
    }
}

/* Occupancy list: caller must hold occ_mutex */
bool is_inside(const char *uid)
{
    for (int i = 0; i < s_inside_n; i++)
        if (strcmp(s_inside[i], uid) == 0) return true;
    return false;
}
bool add_inside(const char *uid)
{
    if (is_inside(uid)) return true;
    if (s_inside_n >= MAX_INSIDE) return false;
    strncpy(s_inside[s_inside_n], uid, UID_MAX_LEN - 1);
    s_inside[s_inside_n][UID_MAX_LEN - 1] = '\0';
    s_inside_n++;
    return true;
}
bool remove_inside(const char *uid)
{
    for (int i = 0; i < s_inside_n; i++) {
        if (strcmp(s_inside[i], uid) == 0) {
            memcpy(s_inside[i], s_inside[s_inside_n - 1], UID_MAX_LEN);
            s_inside_n--;
            return true;
        }
    }
    return false;
}
int occupancy_count(void) { return s_inside_n; }

static void replay_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof line, f)) {
        char *ts  = strtok(line, ",");
        char *uid = strtok(NULL, ",");
        char *dir = strtok(NULL, ",");
        char *res = strtok(NULL, ",\r\n");
        if (!ts || !uid || !dir || !res) continue;
        if (strcmp(res, "GRANTED") != 0 && strcmp(res, "EMERGENCY_OPEN") != 0) continue;
        xSemaphoreTake(occ_mutex, portMAX_DELAY);
        if (strcmp(dir, "IN") == 0) add_inside(uid); else remove_inside(uid);
        xSemaphoreGive(occ_mutex);
    }
    fclose(f);
    rotate_if_needed();
}

/* Call once at boot, before logger_task and other tasks start. */
bool storage_rebuild_occupancy(void)
{
    char p[40];
    for (int i = 1; i < s_next_idx; i++) {
        rotated_path(p, sizeof p, i);
        replay_file(p);
    }
    replay_file(LOG_PATH);
    return true;
}
static const char *result_str(access_result_t r)
{
    switch (r) {
    case RES_GRANTED:          return "GRANTED";
    case RES_UNKNOWN_TAG:      return "UNKNOWN_TAG";
    case RES_OUTSIDE_SCHEDULE: return "OUTSIDE_SCHEDULE";
    case RES_ANTI_PASSBACK:    return "ANTI_PASSBACK";
    case RES_EMERGENCY_OPEN:   return "EMERGENCY_OPEN";
    case RES_LOCKED_TAG:       return "LOCKED_TAG";
    default:                   return "UNKNOWN";
    }
}

static void write_line(const log_event_t *e)
{
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", LOG_PATH);
    } else {
        fprintf(f, "%lld,%s,%s,%s\n",
                (long long)e->scan.ts, e->scan.uid,
                e->scan.is_entry ? "IN" : "OUT",
                result_str(e->result));
        fclose(f);
    }
    xSemaphoreGive(s_sd_mutex);
}

void logger_task(void *arg)
{
    log_event_t e;
    for (;;) {
        if (xQueueReceive(log_queue, &e, portMAX_DELAY) == pdTRUE) {
            write_line(&e);
        }
    }
}
int storage_read_recent(char *buf, size_t len, int n)
{
    if (!buf || len == 0 || n <= 0) return 0;
    if (n > 32) n = 32;
    buf[0] = '\0';

    long pos[32];
    int count = 0;
    char line[96];

    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    FILE *f = fopen(LOG_PATH, "r");
if (!f && s_next_idx > 1) {                 /* live file missing after a rotation */
    char p[40];
    rotated_path(p, sizeof p, s_next_idx - 1);
    f = fopen(p, "r");                       /* fall back to the newest rotated file */
}
if (!f) { xSemaphoreGive(s_sd_mutex); return 0; } 
    for (;;) {
        long p = ftell(f);
        if (!fgets(line, sizeof line, f)) break;
        pos[count % n] = p;       /* ring of the last n line offsets */
        count++;
    }
    int got = count < n ? count : n;
    long start = (count < n) ? 0 : pos[count % n];

    size_t used = 0;
    fseek(f, start, SEEK_SET);
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (used + l + 1 > len) break;
        memcpy(buf + used, line, l);
        used += l;
    }
    buf[used] = '\0';
    fclose(f);
    xSemaphoreGive(s_sd_mutex);
    return got;
}