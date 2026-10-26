
#ifndef ROOM_LIGHTING_H
#define ROOM_LIGHTING_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize automatic room lighting.
 * PIR sensor: GPIO27
 * LED output: GPIO26
 */
esp_err_t room_lighting_init(void);

/**
 * Enable or disable automatic lighting.
 */
void room_lighting_enable(bool enabled);

#endif
