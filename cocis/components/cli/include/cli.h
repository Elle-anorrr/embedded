#pragma once
#include <stdbool.h>

#define CLI_MAX_LINE 128
#define CLI_MAX_ARGS 8

typedef int (*cli_handler_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *help;
    cli_handler_t handler;
    bool admin_only;
} cli_command_t;

void cli_start(void);
/*This is the function member 5 will call to check who may run schedule set and the emergency override.*/

bool cli_iS_admin (void);