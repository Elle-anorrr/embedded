#pragma once
#include <stdbool.h>

#define CLI_MAX_LINE 128
#define CLI_MAX_ARGS 8

typedef int (*cli_handler_t)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *help;
    cli_handler_t handler;
} cli_command_t;

void cli_start(void);