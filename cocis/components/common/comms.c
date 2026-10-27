#include "cocis_common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"

#define MQTT_CONNECTED_BIT BIT0
#define MQTT_PAYLOAD_MAX   192
#define MQTT_TOPIC_MAX     64

static const char *TAG = "comms";
static room_status_t s_room_status[ROOM_COUNT];
static bool s_room_status_valid[ROOM_COUNT];
static EventGroupHandle_t s_comms_events;
static esp_mqtt_client_handle_t s_mqtt_client;
static bool s_setup_attempted;
static bool s_mqtt_started;
static QueueHandle_t s_room_status_queue;

static char s_rx_payload[MQTT_PAYLOAD_MAX];
static char s_rx_topic[MQTT_TOPIC_MAX];
static int s_rx_total_len;
static int s_rx_received_len;
static bool s_rx_in_progress;

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

static bool parse_room_status(const char *topic, const char *payload, size_t len,
                              room_status_t *status)
{
    char expected_topic[MQTT_TOPIC_MAX];
    int topic_len;
    cJSON *root = NULL;
    cJSON *room;
    cJSON *occupied;
    cJSON *light;
    cJSON *timestamp;
    int64_t timestamp_seconds;
    bool valid = false;

    if (!topic || !payload || !status || len == 0 || len >= MQTT_PAYLOAD_MAX) {
        return false;
    }

    for (uint8_t room_id = 1; room_id <= ROOM_COUNT; room_id++) {
        topic_len = snprintf(expected_topic, sizeof(expected_topic),
                             TOPIC_ROOM_STATUS_FMT, room_id);
        if (topic_len > 0 && (size_t)topic_len < sizeof(expected_topic) &&
            strcmp(topic, expected_topic) == 0) {
            root = cJSON_ParseWithLengthOpts(payload, len + 1, NULL, true);
            if (!cJSON_IsObject(root)) {
                goto cleanup;
            }

            room = cJSON_GetObjectItemCaseSensitive(root, "room");
            occupied = cJSON_GetObjectItemCaseSensitive(root, "occupied");
            light = cJSON_GetObjectItemCaseSensitive(root, "light");
            timestamp = cJSON_GetObjectItemCaseSensitive(root, "ts");
            if (!cJSON_IsNumber(room) || !cJSON_IsBool(occupied) ||
                !cJSON_IsBool(light) || !cJSON_IsNumber(timestamp) ||
                !isfinite(room->valuedouble) || room->valuedouble != room_id ||
                !isfinite(timestamp->valuedouble) ||
                timestamp->valuedouble < 0 ||
                timestamp->valuedouble > 9007199254740991.0 ||
                timestamp->valuedouble !=
                    (double)(int64_t)timestamp->valuedouble) {
                goto cleanup;
            }

            timestamp_seconds = (int64_t)timestamp->valuedouble;
            status->ts = (time_t)timestamp_seconds;
            if ((int64_t)status->ts != timestamp_seconds) {
                goto cleanup;
            }
            status->room_id = room_id;
            status->occupied = cJSON_IsTrue(occupied);
            status->light_on = cJSON_IsTrue(light);
            valid = true;
            goto cleanup;
        }
    }

cleanup:
    cJSON_Delete(root);
    return valid;
}

static void cache_room_status(const room_status_t *status)
{
    if (!status || status->room_id == 0 || status->room_id > ROOM_COUNT ||
        !room_mutex) {
        return;
    }

    if (xSemaphoreTake(room_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "could not lock room status cache");
        return;
    }
    s_room_status[status->room_id - 1] = *status;
    s_room_status_valid[status->room_id - 1] = true;
    xSemaphoreGive(room_mutex);
}

static void room_status_cache_task(void *arg)
{
    room_status_t status;
    (void)arg;

    for (;;) {
        if (xQueueReceive(s_room_status_queue, &status, portMAX_DELAY) == pdTRUE) {
            cache_room_status(&status);
        }
    }
}

static void handle_mqtt_data(const esp_mqtt_event_handle_t event)
{
    room_status_t status;

    if (!event) {
        s_rx_in_progress = false;
        return;
    }

    if (event->current_data_offset == 0) {
        s_rx_in_progress = false;
        if (!event->topic || event->topic_len <= 0 ||
            event->topic_len >= MQTT_TOPIC_MAX || event->total_data_len <= 0 ||
            event->total_data_len >= MQTT_PAYLOAD_MAX) {
            return;
        }
        memcpy(s_rx_topic, event->topic, event->topic_len);
        s_rx_topic[event->topic_len] = '\0';
        s_rx_total_len = event->total_data_len;
        s_rx_received_len = 0;
        s_rx_in_progress = true;
    }

    if (!s_rx_in_progress || event->current_data_offset != s_rx_received_len ||
        event->total_data_len != s_rx_total_len || !event->data ||
        event->data_len <= 0 ||
        event->data_len > s_rx_total_len - s_rx_received_len) {
        s_rx_in_progress = false;
        return;
    }

    memcpy(s_rx_payload + s_rx_received_len, event->data, event->data_len);
    s_rx_received_len += event->data_len;
    if (s_rx_received_len != s_rx_total_len) {
        return;
    }

    s_rx_payload[s_rx_total_len] = '\0';
    s_rx_in_progress = false;
    if (parse_room_status(s_rx_topic, s_rx_payload, s_rx_total_len, &status)) {
        if (xQueueSend(s_room_status_queue, &status, 0) != pdTRUE) {
            ESP_LOGW(TAG, "room status cache queue full; dropping update");
        }
    } else {
        ESP_LOGW(TAG, "ignored malformed room status message");
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    (void)arg;
    (void)event_base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(s_comms_events, MQTT_CONNECTED_BIT);
        for (uint8_t room_id = 1; room_id <= ROOM_COUNT; room_id++) {
            char topic[MQTT_TOPIC_MAX];
            int len = snprintf(topic, sizeof(topic), TOPIC_ROOM_STATUS_FMT, room_id);
            if (len <= 0 || (size_t)len >= sizeof(topic) ||
                esp_mqtt_client_subscribe(s_mqtt_client, topic, 1) < 0) {
            ESP_LOGE(TAG, "failed to subscribe to room %u status",
                     (unsigned)room_id);
            }
        }
        ESP_LOGI(TAG, "connected and subscribed to room status topics");
        break;

    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(s_comms_events, MQTT_CONNECTED_BIT);
        s_rx_in_progress = false;
        ESP_LOGW(TAG, "MQTT disconnected; client will reconnect");
        break;

    case MQTT_EVENT_DATA:
        handle_mqtt_data(event);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT transport error");
        break;

    default:
        break;
    }
}

static void start_mqtt_client(void)
{
    if (s_mqtt_started) {
        return;
    }

    esp_mqtt_client_config_t config = {
        .broker.address.uri = MQTT_BROKER_URI,
    };
    s_mqtt_client = esp_mqtt_client_init(&config);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "could not create MQTT client");
        return;
    }

    esp_err_t err = esp_mqtt_client_register_event(
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not register MQTT event handler: %s",
                 esp_err_to_name(err));
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return;
    }

    err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return;
    }
    s_mqtt_started = true;
}

static void network_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi connect request failed: %s", esp_err_to_name(err));
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_comms_events, MQTT_CONNECTED_BIT);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi reconnect request failed: %s", esp_err_to_name(err));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        start_mqtt_client();
    }
}

void comms_init(void)
{
    if (!room_mutex) {
        ESP_LOGE(TAG, "common_init must run before comms_init");
        return;
    }
    if (s_setup_attempted) {
        return;
    }
    s_setup_attempted = true;

    s_comms_events = xEventGroupCreate();
    s_room_status_queue = xQueueCreate(ROOM_COUNT * 2, sizeof(room_status_t));
    if (!s_comms_events || !s_room_status_queue) {
        ESP_LOGE(TAG, "could not create MQTT coordination objects");
        return;
    }
    if (xTaskCreate(room_status_cache_task, "room_cache", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not create room status cache task");
        return;
    }

    if (xSemaphoreTake(room_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "could not lock room status cache during initialization");
        return;
    }
    for (uint8_t room_id = 1; room_id <= ROOM_COUNT; room_id++) {
        s_room_status[room_id - 1].room_id = room_id;
        s_room_status[room_id - 1].occupied = false;
        s_room_status[room_id - 1].light_on = false;
        s_room_status[room_id - 1].ts = 0;
        s_room_status_valid[room_id - 1] = false;
    }
    xSemaphoreGive(room_mutex);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "network interface init failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "default event loop init failed: %s", esp_err_to_name(err));
        return;
    }

    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(TAG, "could not create default Wi-Fi station interface");
        return;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_init_config.nvs_enable = false;
    err = esp_wifi_init(&wifi_init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     network_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         network_event_handler, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network event registration failed: %s", esp_err_to_name(err));
        return;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi station started");
}

void comms_publish_room(const room_status_t *s)
{
    char topic[MQTT_TOPIC_MAX];
    char payload[MQTT_PAYLOAD_MAX];
    int topic_len;
    int payload_len;

    if (!s || s->room_id == 0 || s->room_id > ROOM_COUNT) {
        ESP_LOGW(TAG, "ignored invalid room status publish request");
        return;
    }
    if (!s_mqtt_client || !s_comms_events ||
        !(xEventGroupGetBits(s_comms_events) & MQTT_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "room status not published: MQTT is disconnected");
        return;
    }

    topic_len = snprintf(topic, sizeof(topic), TOPIC_ROOM_STATUS_FMT, s->room_id);
    payload_len = snprintf(payload, sizeof(payload),
                           "{\"room\":%u,\"occupied\":%s,\"light\":%s,\"ts\":%lld}",
                           s->room_id, s->occupied ? "true" : "false",
                           s->light_on ? "true" : "false", (long long)s->ts);
    if (topic_len <= 0 || (size_t)topic_len >= sizeof(topic) ||
        payload_len <= 0 || (size_t)payload_len >= sizeof(payload)) {
        ESP_LOGE(TAG, "room status message exceeds buffer capacity");
        return;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload,
                                         payload_len, 1, 1);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "failed to publish room %u status", (unsigned)s->room_id);
    }
}

bool comms_get_room(uint8_t room_id, room_status_t *out)
{
    if (!out || room_id == 0 || room_id > ROOM_COUNT || !room_mutex) {
        return false;
    }

    if (xSemaphoreTake(room_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool valid = s_room_status_valid[room_id - 1];
    if (valid) {
        *out = s_room_status[room_id - 1];
    }
    xSemaphoreGive(room_mutex);
    return valid;
}
