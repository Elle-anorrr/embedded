#ifndef DOOR_H
#define DOOR_H

/* Sets up servos, buzzer and LEDs and starts their tasks.
 * door_open() and buzzer_deny() themselves are declared in cocis_common.h. */
void door_init(void);

#endif