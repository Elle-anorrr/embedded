/*
 * door.c - door servos, buzzer and status LEDs (Person 1).
 *
 * Design: nothing here blocks the caller.
 *   door_open() / buzzer_deny() just drop a message in a queue and return.
 *   - two door tasks (one per door) own the servos
 *   - one feedback task owns the buzzer and both LEDs
 * Each piece of hardware has exactly ONE owner task, so no two tasks ever
 * drive the same pin and no mutex is needed.
 */
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cocis_common.h"
#include "door.h"

static const char *TAG = "door";

/* ---- tuning ---- */
#define DOOR_OPEN_DEG     90
#define DOOR_CLOSED_DEG    0
#define DOOR_OPEN_MS    3000      /* how long a door stays open */
#define SERVO_MIN_US     500      /* pulse for 0 degrees   */
#define SERVO_MAX_US    2500      /* pulse for 180 degrees */
#define SERVO_PERIOD_US 20000     /* 50 Hz */
#define BUZZER_FREQ_HZ  2000

#define DOOR_ENTRY 0
#define DOOR_EXIT  1

typedef enum { FB_GRANT, FB_DENY } feedback_t;

static const int s_servo_pin[2] = { PIN_SERVO_IN, PIN_SERVO_OUT };
static const ledc_channel_t s_servo_ch[2] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1 };

static QueueHandle_t s_door_q[2];
static QueueHandle_t s_fb_q;

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                   */
/* ------------------------------------------------------------------ */

/* Servo angle -> pulse width -> LEDC duty value (14-bit counter) */
static void servo_set_angle(int door, int deg)
{
    uint32_t pulse_us = SERVO_MIN_US + (SERVO_MAX_US - SERVO_MIN_US) * deg / 180;
    uint32_t duty = (pulse_us * (1u << 14)) / SERVO_PERIOD_US;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, s_servo_ch[door], duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, s_servo_ch[door]);
}

static void buzzer_set(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, on ? 512 : 0);  /* 50% of 10-bit */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

static void beep(int ms)
{
    buzzer_set(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    buzzer_set(false);
}

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */
static void door_task(void *arg)
{
    int door = (int)(intptr_t)arg;
    uint8_t cmd;
    while (1) {
        if (xQueueReceive(s_door_q[door], &cmd, portMAX_DELAY)) {
            servo_set_angle(door, DOOR_OPEN_DEG);
            vTaskDelay(pdMS_TO_TICKS(DOOR_OPEN_MS));
            servo_set_angle(door, DOOR_CLOSED_DEG);
            xQueueReset(s_door_q[door]);   /* grants that arrived while open are redundant */
        }
    }
}

static void feedback_task(void *arg)
{
    feedback_t ev;
    while (1) {
        if (xQueueReceive(s_fb_q, &ev, portMAX_DELAY)) {
            if (ev == FB_GRANT) {
                gpio_set_level(PIN_LED_GREEN, 1);
                beep(100);
                vTaskDelay(pdMS_TO_TICKS(900));
                gpio_set_level(PIN_LED_GREEN, 0);
            } else {
                for (int i = 0; i < 3; i++) {
                    gpio_set_level(PIN_LED_RED, 1);
                    beep(150);
                    gpio_set_level(PIN_LED_RED, 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void door_init(void)
{
    /* LEDs */
    int leds[2] = { PIN_LED_GREEN, PIN_LED_RED };
    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(leds[i]);
        gpio_set_direction(leds[i], GPIO_MODE_OUTPUT);
        gpio_set_level(leds[i], 0);
    }

    /* Timer 0: 50 Hz for the servos */
    ledc_timer_config_t servo_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&servo_timer));

    /* Timer 1: 2 kHz tone for the buzzer */
    ledc_timer_config_t buz_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&buz_timer));

    for (int d = 0; d < 2; d++) {
        ledc_channel_config_t ch = {
            .gpio_num = s_servo_pin[d],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = s_servo_ch[d],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
        servo_set_angle(d, DOOR_CLOSED_DEG);   /* start closed */
    }

    ledc_channel_config_t buz = {
        .gpio_num = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_2,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&buz));

    /* Queues and tasks */
    for (int d = 0; d < 2; d++) {
        s_door_q[d] = xQueueCreate(1, sizeof(uint8_t));
        configASSERT(s_door_q[d]);
    }
    s_fb_q = xQueueCreate(4, sizeof(feedback_t));
    configASSERT(s_fb_q);

    xTaskCreate(door_task, "door_in",  2048, (void *)(intptr_t)DOOR_ENTRY, 5, NULL);
    xTaskCreate(door_task, "door_out", 2048, (void *)(intptr_t)DOOR_EXIT,  5, NULL);
    xTaskCreate(feedback_task, "feedback", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "doors, buzzer and LEDs ready");
}

void door_open(bool entry_door)
{
    if (!s_fb_q) return;                       /* door_init() not called yet */
    uint8_t cmd = 1;
    feedback_t ev = FB_GRANT;
    xQueueSend(s_door_q[entry_door ? DOOR_ENTRY : DOOR_EXIT], &cmd, 0);
    xQueueSend(s_fb_q, &ev, 0);
}

void buzzer_deny(void)
{
    if (!s_fb_q) return;
    feedback_t ev = FB_DENY;
    xQueueSend(s_fb_q, &ev, 0);
}