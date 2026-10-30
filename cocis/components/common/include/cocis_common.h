/*
 * cocis_common.h
 * Shared contract for the CoCIS Smart Control and Monitoring System.
 * Every module is coded against this file. Change it only by group agreement.
 */
#ifndef COCIS_COMMON_H
#define COCIS_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/* ------------------------------------------------------------------ */
/* 1. PIN MAPS (adjust to match your Wokwi diagram.json)               */
/* ------------------------------------------------------------------ */

/* ---- Entrance board ---- */
#define PIN_SPI_SCK        18
#define PIN_SPI_MISO       19
#define PIN_SPI_MOSI       23
#define PIN_SD_CS           5
#define PIN_RFID_IN_CS     21
#define PIN_RFID_IN_RST    16
#define PIN_RFID_OUT_CS    22
#define PIN_RFID_OUT_RST   17
#define PIN_RTC_SDA         4
#define PIN_RTC_SCL        15
#define PIN_SERVO_IN       25
#define PIN_SERVO_OUT      26
#define PIN_BUZZER         27
#define PIN_LED_GREEN      32
#define PIN_LED_RED        33

/* ---- Room boards (same pins on both) ---- */
#define PIN_PIR            13
#define PIN_LIGHT           2    /* LED or relay */
#define PIN_LDR_ADC        34    /* optional daylight sensor */

/* ------------------------------------------------------------------ */
/* 2. LIMITS                                                           */
/* ------------------------------------------------------------------ */
#define UID_MAX_LEN        16
#define MAX_TAGS           50
#define MAX_INSIDE         50
#define ROOM_COUNT          2
#define ROOM_LIGHT_TIMEOUT_S  60    /* lights stay on this long after last motion */
#define LOGIN_MAX_FAILS     3
#define LOGIN_LOCKOUT_S    60

/* ------------------------------------------------------------------ */
/* 3. DATA TYPES                                                       */
/* ------------------------------------------------------------------ */

/* A raw tag scan from either reader */
typedef struct {
    char   uid[UID_MAX_LEN];   /* hex string e.g. "A1B2C3D4" */
    bool   is_entry;           /* true = entry reader, false = exit reader */
    time_t ts;
} scan_t;

/* Why a scan was accepted or denied */
typedef enum {
    RES_GRANTED = 0,
    RES_UNKNOWN_TAG,
    RES_OUTSIDE_SCHEDULE,
    RES_ANTI_PASSBACK,      /* entering while already inside, or exiting while not inside */
    RES_EMERGENCY_OPEN,     /* admin emergency override */
    RES_LOCKED_TAG          /* tag blocked after repeated denials (tamper) */
} access_result_t;

/* One entry in the SD log */
typedef struct {
    scan_t          scan;
    access_result_t result;
} log_event_t;

/* Building access window, minutes since midnight (e.g. 07:00 = 420) */
typedef struct {
    uint16_t start_min;
    uint16_t end_min;
    bool     enabled;
} schedule_t;

/* Status a room board reports to the entrance board */
typedef struct {
    uint8_t room_id;       /* 1 or 2 */
    bool    occupied;
    bool    light_on;
    time_t  ts;
} room_status_t;

/* ------------------------------------------------------------------ */
/* 4. SHARED RTOS OBJECTS (entrance board). Created in app_main().     */
/* ------------------------------------------------------------------ */
extern QueueHandle_t     scan_queue;   /* scan_t       : rfid tasks -> access_task   */
extern QueueHandle_t     log_queue;    /* log_event_t  : access_task -> logger_task  */
extern SemaphoreHandle_t occ_mutex;    /* guards the "who is inside" list            */
extern SemaphoreHandle_t cfg_mutex;    /* guards schedule + tag list + admin config  */
extern SemaphoreHandle_t room_mutex;   /* guards room_status[]                       */

/* Defined in common.c. Call FIRST in app_main(), before any task starts. */
void common_init(void);

/*
 * RULES (these are your synchronization story for the report):
 *  - Only logger_task touches the SD card. Everyone else sends to log_queue.
 *  - Always take a mutex before reading OR writing the data it guards.
 *  - Never hold two mutexes at once. If you must, always take
 *    cfg_mutex before occ_mutex to avoid deadlock.
 *  - Never block on a mutex inside an ISR.
 */

/* ------------------------------------------------------------------ */
/* 5. MODULE FUNCTION CONTRACTS                                        */
/* ------------------------------------------------------------------ */

/* ---- Person 1: access control ---- */
void access_init(void);
void door_open(bool entry_door);          /* opens servo, closes after ~3 s */
void buzzer_deny(void);
void access_task(void *arg);

/* ---- Person 2: storage and logging ---- */
bool storage_init(void);                                  /* mount SD */
void logger_task(void *arg);                              /* sole SD writer */
int  storage_read_recent(char *buf, size_t len, int n);   /* last n log lines as text */
bool storage_rebuild_occupancy(void);                     /* replay log on boot */

/* Occupancy list helpers (caller must hold occ_mutex) */
bool is_inside(const char *uid);
bool add_inside(const char *uid);
bool remove_inside(const char *uid);
int  occupancy_count(void);

/* ---- Person 3: room firmware (runs on room boards) ---- */
void room_init(uint8_t room_id);
void pir_task(void *arg);
void light_task(void *arg);

/* ---- Person 4: CLI and auth ---- */
bool auth_login(const char *user, const char *pass);   /* handles lockout */
bool auth_is_admin(void);
void auth_logout(void);

/* ---- Person 5: comms and schedule ---- */
void comms_init(void);                                 /* wifi + mqtt */
void comms_publish_room(const room_status_t *s);       /* room boards call this */
bool comms_get_room(uint8_t room_id, room_status_t *out); /* entrance reads cached state */
bool ds1307_rtc_init(void);
bool rtc_get_time(time_t *out);
bool within_schedule(time_t now);                      /* checks cfg under cfg_mutex */
bool access_is_allowed(bool is_entry, bool currently_inside, time_t now);
bool schedule_set(uint16_t start_min, uint16_t end_min);  /* admin only, caller checks auth */
bool schedule_get(schedule_t *out);
bool tag_is_registered(const char *uid);
bool tag_add(const char *uid);
bool tag_remove(const char *uid);

/* ------------------------------------------------------------------ */
/* 6. MQTT                                                             */
/* ------------------------------------------------------------------ */
#define WIFI_SSID          "Wokwi-GUEST"
#define WIFI_PASS          ""
#define MQTT_BROKER_URI    "mqtt://broker.hivemq.com"

#define TOPIC_ROOM_STATUS_FMT  "cocis/room%d/status"   /* room -> entrance */
#define TOPIC_ENTRANCE_CMD     "cocis/entrance/cmd"    /* optional: dashboard -> entrance */

/* Room status payload (JSON, retained):
 *   {"room":1,"occupied":true,"light":true,"ts":1761120000}
 */

#endif /* COCIS_COMMON_H */