#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cocis_common.h"
#include <dirent.h>

static const char *TAG = "storage_test";
static void find_next_idx(void);

static void send(const char *uid, bool in, access_result_t r)
{
    log_event_t e = { .result = r };
    strcpy(e.scan.uid, uid);
    e.scan.is_entry = in;
    e.scan.ts = 1761120000;
    xQueueSend(log_queue, &e, portMAX_DELAY);
}

void app_main(void)
{
    common_init();
    if (!storage_init()) return;
    storage_rebuild_occupancy();
    ESP_LOGI(TAG, "after boot rebuild: %d inside", occupancy_count());
    xTaskCreate(logger_task, "logger", 4096, NULL, 5, NULL);

    if (esp_reset_reason() != ESP_RST_SW) {        /* first boot only */
        send("A1B2C3D4", true,  RES_GRANTED);
        send("E5F6A7B8", true,  RES_GRANTED);
        send("A1B2C3D4", false, RES_GRANTED);
        for (int i = 0; i < 80; i++) send("ZZ", true, RES_UNKNOWN_TAG);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart(); 
                                     /* simulate a reboot */
    }else {
    static char buf[512];
    int n = storage_read_recent(buf, sizeof buf, 3);
    ESP_LOGI(TAG, "last %d lines:\n%s", n, buf);

     DIR *d = opendir("/sdcard");
        struct dirent *de;
        while (d && (de = readdir(d))) ESP_LOGI(TAG, "file: %s", de->d_name);
        if (d) closedir(d);
    }    
}
    