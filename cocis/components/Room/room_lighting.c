
#include "room_lighting.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#define PIR_PIN GPIO_NUM_27
#define LED_PIN GPIO_NUM_26
#define INACTIVITY_MS 60000
#define TASK_STACK_SIZE 3072
#define TASK_PRIORITY 5

static const char *TAG = "ROOM_LIGHTING";

static bool lighting_enabled = true;
static bool light_on = false;
static int64_t last_motion_ms = 0;

static void lighting_task(void *arg)
{
    int previous_motion = -1;

    while (1) {
        int motion = gpio_get_level(PIR_PIN);
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (motion != previous_motion) {
            ESP_LOGI(TAG, "PIR state: %d", motion);
            previous_motion = motion;
        }

        if (lighting_enabled && motion == 1) {
            last_motion_ms = now_ms;

            if (!light_on) {
                gpio_set_level(LED_PIN, 1);
                light_on = true;
                ESP_LOGI(TAG, "Motion detected: LIGHT ON");
            }
        }

        if (light_on &&
            (!lighting_enabled ||
             (motion == 0 &&
              now_ms - last_motion_ms >= INACTIVITY_MS))) {
            gpio_set_level(LED_PIN, 0);
            light_on = false;
            ESP_LOGI(TAG, "LIGHT OFF");
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

esp_err_t room_lighting_init(void)
{
    gpio_config_t pir_config = {
        .pin_bit_mask = (1ULL << PIR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t err = gpio_config(&pir_config);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    err = gpio_config(&led_config);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_level(LED_PIN, 0);
    if (err != ESP_OK) {
        return err;
    }

    light_on = false;
    last_motion_ms = 0;

    BaseType_t task_result = xTaskCreate(
        lighting_task,
        "room_lighting",
        TASK_STACK_SIZE,
        NULL,
        TASK_PRIORITY,
        NULL
    );

    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Automatic room lighting initialized");
    return ESP_OK;
}

void room_lighting_enable(bool enabled)
{
    lighting_enabled = enabled;

    if (!enabled && light_on) {
        gpio_set_level(LED_PIN, 0);
        light_on = false;
        ESP_LOGI(TAG, "Automatic lighting disabled");
    }
}
