/* Test for steps 1-3: two readers + doors + buzzer + LEDs.
 * One hard-coded "valid" tag. Replaced by the real app_main() at integration. */
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cocis_common.h"
#include "rfid.h"
#include "door.h"

static const char *TAG = "test";
#define TEST_VALID_UID "01020304"

void app_main(void)
{
    common_init();
    ESP_ERROR_CHECK(rfid_bus_init());
    ESP_ERROR_CHECK(rfid_init(RFID_IN,  PIN_RFID_IN_CS,  PIN_RFID_IN_RST));
    ESP_ERROR_CHECK(rfid_init(RFID_OUT, PIN_RFID_OUT_CS, PIN_RFID_OUT_RST));
    door_init();

    char uid[UID_MAX_LEN];
    while (1) {
        for (int r = 0; r < RFID_COUNT; r++) {
            if (rfid_read_uid((rfid_id_t)r, uid, sizeof(uid))) {
                bool entry = (r == RFID_IN);
                ESP_LOGI(TAG, "%s scan: %s", entry ? "IN" : "OUT", uid);
                if (strcmp(uid, TEST_VALID_UID) == 0) door_open(entry);
                else buzzer_deny();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}