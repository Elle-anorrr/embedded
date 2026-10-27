#include "cli.h"
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"
#include "psa/crypto.h"

#define CLI_UART UART_NUM_0

static int cmd_help(int argc, char **argv);
static int cmd_ping(int argc, char **argv);
static int cmd_login(int argc, char **argv);
static int cmd_logout(int argc, char **argv);
static int cmd_passwd (int argc, char **argv);

/* One line per command. */
static const cli_command_t commands[] = {
    {"help", "List all commands",       cmd_help , false },
    {"ping", "Check the CLI is alive",  cmd_ping , false },
    {"login",  "Admin login: login <password>", cmd_login, false },
    {"logout", "Log out of admin mode",   cmd_logout , false },
    {"passwd", "Change admin password: passwd <new>", cmd_passwd , true},
};
#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static int cmd_help(int argc, char **argv)
{
    printf("Available commands:\r\n");
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        printf("  %-10s %s%s\r\n", commands[i].name, commands[i].help ,commands[i].admin_only ? "[admin]" :"");
    }
    return 0;
}

static int cmd_ping(int argc, char **argv)
{
    printf("pong\r\n");
    return 0;
}

#define DEFAULT_PASSWORD "admin123"
#define NVS_NS     "cli_auth"
#define SALT_LEN   16
#define HASH_LEN   32

static uint8_t salt[SALT_LEN];
static uint8_t stored_hash[HASH_LEN];
static bool auth_ready = false;

static bool hash_password(const uint8_t *s, const char *pw, uint8_t *out)
{
    uint8_t buf[SALT_LEN + CLI_MAX_LINE];
    size_t pwlen = strlen(pw);
    if (pwlen >= CLI_MAX_LINE) {
        return false;
    }
    memcpy(buf, s, SALT_LEN);
    memcpy(buf + SALT_LEN, pw, pwlen);
    size_t out_len = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, buf, SALT_LEN + pwlen,
                            out, HASH_LEN, &out_len) == PSA_SUCCESS;
}

static bool hashes_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < HASH_LEN; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

static void auth_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK || psa_crypto_init() != PSA_SUCCESS) {
        printf("Auth init failed: login disabled.\r\n");
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        printf("Auth storage failed: login disabled.\r\n");
        return;
    }

    size_t slen = SALT_LEN, hlen = HASH_LEN;
    if (nvs_get_blob(h, "salt", salt, &slen) != ESP_OK ||
        nvs_get_blob(h, "hash", stored_hash, &hlen) != ESP_OK) {
        esp_fill_random(salt, SALT_LEN);
        hash_password(salt, DEFAULT_PASSWORD, stored_hash);
        nvs_set_blob(h, "salt", salt, SALT_LEN);
        nvs_set_blob(h, "hash", stored_hash, HASH_LEN);
        nvs_commit(h);
        printf("First boot: default admin password stored.\r\n");
    }
    nvs_close(h);
    auth_ready = true;
}
#define MAX_FAILED          3
#define LOCKOUT_MS          30000

static bool logged_in = false;
static int failed_attempts = 0;
static bool locked = false;
static TickType_t locked_at = 0;

bool cli_is_admin(void)
{
    return logged_in;
}

static int cmd_login(int argc, char **argv)
{
    if (logged_in) {
        printf("Already logged in.\r\n");
        return 0;
    }

    if (locked) {
        uint32_t elapsed_ms = (xTaskGetTickCount() - locked_at) * portTICK_PERIOD_MS;
        if (elapsed_ms < LOCKOUT_MS) {
            printf("Locked. Try again in %lu s.\r\n",
                   (unsigned long)((LOCKOUT_MS - elapsed_ms) / 1000 + 1));
            return -1;
        }
        locked = false;
        failed_attempts = 0;
    }

    if (argc != 2) {
        printf("Usage: login <password>\r\n");
        return -1;
    }

    uint8_t h[HASH_LEN];
    if (auth_ready && hash_password(salt, argv[1], h) && hashes_equal(h, stored_hash)) {
        logged_in = true;
        failed_attempts = 0;
        printf("Login successful.\r\n");
        return 0;
    }

    failed_attempts++;
    if (failed_attempts >= MAX_FAILED) {
        locked = true;
        locked_at = xTaskGetTickCount();
        printf("Too many failed attempts. Locked for %d seconds.\r\n", LOCKOUT_MS / 1000);
    } else {
        printf("Wrong password. %d attempt(s) left.\r\n", MAX_FAILED - failed_attempts);
    }
    return -1;
}

static int cmd_logout(int argc, char **argv)
{
    logged_in = false;
    printf("Logged out.\r\n");
    return 0;
}

#define MIN_PASSWORD_LEN 6

static int cmd_passwd(int argc, char **argv)
{
    if (!logged_in) {
        printf("Permission denied. Login first.\r\n");
        return -1;
    }
    if (argc != 2) {
        printf("Usage: passwd <new password> (no spaces)\r\n");
        return -1;
    }
    if (strlen(argv[1]) < MIN_PASSWORD_LEN) {
        printf("Too short. Minimum %d characters.\r\n", MIN_PASSWORD_LEN);
        return -1;
    }

    uint8_t new_salt[SALT_LEN];
    uint8_t new_hash[HASH_LEN];
    esp_fill_random(new_salt, SALT_LEN);
    if (!hash_password(new_salt, argv[1], new_hash)) {
        printf("Could not hash the password.\r\n");
        return -1;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        printf("Could not open storage. Password unchanged.\r\n");
        return -1;
    }
    esp_err_t err = nvs_set_blob(h, "salt", new_salt, SALT_LEN);
    if (err == ESP_OK) err = nvs_set_blob(h, "hash", new_hash, HASH_LEN);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        printf("Save failed. Password unchanged.\r\n");
        return -1;
    }

    /* Only switch to the new values after they were saved. */
    memcpy(salt, new_salt, SALT_LEN);
    memcpy(stored_hash, new_hash, HASH_LEN);
    printf("Password changed.\r\n");
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
            if (commands[i].admin_only && !cli_is_admin()) {
                printf("Permission denied. Login first.\r\n");
                return;
            }
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
    auth_init();
    uart_driver_install(CLI_UART, 256, 0, 0, NULL, 0);
    xTaskCreate(cli_task, "cli", 4096, NULL, 5, NULL);
}