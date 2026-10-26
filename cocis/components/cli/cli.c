#include "cli.h"
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CLI_UART UART_NUM_0

static int cmd_help(int argc, char **argv);
static int cmd_ping(int argc, char **argv);

/* One line per command. */
static const cli_command_t commands[] = {
    {"help", "List all commands",       cmd_help},
    {"ping", "Check the CLI is alive",  cmd_ping},
};
#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static int cmd_help(int argc, char **argv)
{
    printf("Available commands:\r\n");
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        printf("  %-10s %s\r\n", commands[i].name, commands[i].help);
    }
    return 0;
}

static int cmd_ping(int argc, char **argv)
{
    printf("pong\r\n");
    return 0;
}

static void print_prompt(void)
{
    printf("cocis> ");
    fflush(stdout);
}

/* Split the line into words, find the command, run it. */
static void cli_execute(char *line)
{
    char *argv[CLI_MAX_ARGS];
    int argc = 0;

    char *tok = strtok(line, " ");
    while (tok != NULL && argc < CLI_MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok(NULL, " ");
    }
    if (argc == 0) {
        return;
    }

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].handler(argc, argv);
            return;
        }
    }
    printf("Unknown command: %s (type 'help')\r\n", argv[0]);
}

static void cli_task(void *arg)
{
    char line[CLI_MAX_LINE];
    int len = 0;
    uint8_t c;
    uint8_t prev = 0;

    printf("\r\nCoCIS CLI ready. Type 'help'.\r\n");
    print_prompt();

    while (1) {
        if (uart_read_bytes(CLI_UART, &c, 1, portMAX_DELAY) <= 0) {
            continue;
        }

        if (c == '\n' && prev == '\r') {   /* ignore the \n of a \r\n pair */
            prev = c;
            continue;
        }
        prev = c;

        if (c == '\r' || c == '\n') {
            printf("\r\n");
            line[len] = '\0';
            cli_execute(line);
            len = 0;
            print_prompt();
        } else if (c == 0x08 || c == 0x7F) {          /* backspace */
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c >= 32 && c < 127 && len < CLI_MAX_LINE - 1) {
            line[len++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
}

void cli_start(void)
{
    uart_driver_install(CLI_UART, 256, 0, 0, NULL, 0);
    xTaskCreate(cli_task, "cli", 4096, NULL, 5, NULL);
}