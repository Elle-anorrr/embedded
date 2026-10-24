#ifndef RFID_H
#define RFID_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    RFID_IN = 0,     /* entry reader */
    RFID_OUT = 1,    /* exit reader  */
    RFID_COUNT
} rfid_id_t;

/* Initialise the shared SPI bus. Safe to call more than once (the SD card
 * module may call it too); an "already initialised" result counts as success. */
esp_err_t rfid_bus_init(void);

/* Attach one MFRC522 reader to the bus and configure it. */
esp_err_t rfid_init(rfid_id_t id, int cs_pin, int rst_pin);

/* Non-blocking poll. Returns true and fills uid_out with an 8-character hex
 * string (e.g. "01020304") if a new card was read. Returns false if no card. */
bool rfid_read_uid(rfid_id_t id, char *uid_out, size_t len);

#endif