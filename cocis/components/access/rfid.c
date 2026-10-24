/*
 * rfid.c - minimal MFRC522 driver over SPI for ESP-IDF.
 * Supports 4-byte UIDs only (set the Wokwi card uid to 4 bytes).
 * NOTE: written from the MFRC522 datasheet; verify in Wokwi.
 */
#include <stdio.h>
#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rfid.h"
#include "cocis_common.h"

static const char *TAG = "rfid";

/* ---- MFRC522 registers ---- */
#define REG_COMMAND     0x01
#define REG_COMIRQ      0x04
#define REG_DIVIRQ      0x05
#define REG_ERROR       0x06
#define REG_FIFODATA    0x09
#define REG_FIFOLEVEL   0x0A
#define REG_BITFRAMING  0x0D
#define REG_COLL        0x0E
#define REG_MODE        0x11
#define REG_TXCONTROL   0x14
#define REG_TXASK       0x15
#define REG_CRC_H       0x21
#define REG_CRC_L       0x22
#define REG_TMODE       0x2A
#define REG_TPRESCALER  0x2B
#define REG_TRELOAD_H   0x2C
#define REG_TRELOAD_L   0x2D
#define REG_VERSION     0x37

/* ---- reader commands ---- */
#define CMD_IDLE        0x00
#define CMD_CALCCRC     0x03
#define CMD_TRANSCEIVE  0x0C
#define CMD_SOFTRESET   0x0F

/* ---- card commands ---- */
/* 0x26 = REQA (only wakes idle cards, so a halted card is read once per
 * presentation). If Wokwi never re-reads a card, try 0x52 (WUPA) and rely
 * on a software cooldown instead. */
#define PICC_WAKE_CMD   0x26
#define PICC_SEL_CL1    0x93
#define PICC_HALT       0x50

static spi_device_handle_t s_dev[RFID_COUNT];

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */
static void reg_write(rfid_id_t id, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_transmit(s_dev[id], &t);
}

static uint8_t reg_read(rfid_id_t id, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t rx[2] = { 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_transmit(s_dev[id], &t);
    return rx[1];
}

static void reg_set_bits(rfid_id_t id, uint8_t reg, uint8_t mask)
{
    reg_write(id, reg, reg_read(id, reg) | mask);
}

static void reg_clear_bits(rfid_id_t id, uint8_t reg, uint8_t mask)
{
    reg_write(id, reg, reg_read(id, reg) & (uint8_t)~mask);
}

/* ------------------------------------------------------------------ */
/* Talking to a card                                                   */
/* ------------------------------------------------------------------ */

/* Send tx bytes to the card, collect its reply in rx.
 * rx_len: in = size of rx buffer, out = bytes received.
 * tx_last_bits: number of valid bits in the last byte sent (0 = all 8). */
static bool transceive(rfid_id_t id, const uint8_t *tx, uint8_t tx_len,
                       uint8_t *rx, uint8_t *rx_len, uint8_t tx_last_bits)
{
    reg_write(id, REG_COMMAND, CMD_IDLE);
    reg_write(id, REG_COMIRQ, 0x7F);          /* clear all interrupt flags */
    reg_write(id, REG_FIFOLEVEL, 0x80);       /* flush FIFO */
    for (uint8_t i = 0; i < tx_len; i++) {
        reg_write(id, REG_FIFODATA, tx[i]);
    }
    reg_write(id, REG_BITFRAMING, tx_last_bits);
    reg_write(id, REG_COMMAND, CMD_TRANSCEIVE);
    reg_set_bits(id, REG_BITFRAMING, 0x80);   /* StartSend */

    int64_t deadline = esp_timer_get_time() + 30000;   /* 30 ms */
    uint8_t irq = 0;
    while (esp_timer_get_time() < deadline) {
        irq = reg_read(id, REG_COMIRQ);
        if (irq & 0x31) break;   /* RxIRq | IdleIRq | TimerIRq */
        taskYIELD();
    }
    reg_clear_bits(id, REG_BITFRAMING, 0x80); /* stop sending */

    if (!(irq & 0x30)) return false;          /* timeout: no card answered */
    if (reg_read(id, REG_ERROR) & 0x13) return false; /* buffer/parity/protocol error */

    uint8_t n = reg_read(id, REG_FIFOLEVEL);
    if (rx && rx_len) {
        if (n > *rx_len) return false;
        for (uint8_t i = 0; i < n; i++) rx[i] = reg_read(id, REG_FIFODATA);
        *rx_len = n;
    }
    return true;
}

/* Let the reader chip compute the ISO14443A CRC. */
static bool calc_crc(rfid_id_t id, const uint8_t *data, uint8_t len, uint8_t out[2])
{
    reg_write(id, REG_COMMAND, CMD_IDLE);
    reg_write(id, REG_DIVIRQ, 0x04);          /* clear CRCIRq */
    reg_write(id, REG_FIFOLEVEL, 0x80);
    for (uint8_t i = 0; i < len; i++) reg_write(id, REG_FIFODATA, data[i]);
    reg_write(id, REG_COMMAND, CMD_CALCCRC);

    int64_t deadline = esp_timer_get_time() + 20000;
    while (!(reg_read(id, REG_DIVIRQ) & 0x04)) {
        if (esp_timer_get_time() > deadline) return false;
        taskYIELD();
    }
    reg_write(id, REG_COMMAND, CMD_IDLE);
    out[0] = reg_read(id, REG_CRC_L);
    out[1] = reg_read(id, REG_CRC_H);
    return true;
}

/* Step 1: is there a card in the field? */
static bool picc_request(rfid_id_t id)
{
    reg_clear_bits(id, REG_COLL, 0x80);
    uint8_t cmd = PICC_WAKE_CMD;
    uint8_t atqa[2];
    uint8_t len = sizeof(atqa);
    return transceive(id, &cmd, 1, atqa, &len, 0x07) && len == 2;
}

/* Step 2: anti-collision + select. Fills uid[4]. */
static bool picc_select(rfid_id_t id, uint8_t uid[4])
{
    uint8_t ac[2] = { PICC_SEL_CL1, 0x20 };
    uint8_t buf[5];
    uint8_t len = sizeof(buf);
    if (!transceive(id, ac, 2, buf, &len, 0) || len != 5) return false;

    uint8_t bcc = buf[0] ^ buf[1] ^ buf[2] ^ buf[3];
    if (bcc != buf[4]) return false;          /* corrupted read */
    if (buf[0] == 0x88) return false;         /* 7-byte UID: not supported */

    uint8_t sel[9] = { PICC_SEL_CL1, 0x70, buf[0], buf[1], buf[2], buf[3], buf[4] };
    if (!calc_crc(id, sel, 7, &sel[7])) return false;

    uint8_t sak[3];
    len = sizeof(sak);
    if (!transceive(id, sel, 9, sak, &len, 0)) return false;

    memcpy(uid, buf, 4);
    return true;
}

/* Step 3: put the card to sleep so it isn't read again until re-presented. */
static void picc_halt(rfid_id_t id)
{
    uint8_t buf[4] = { PICC_HALT, 0x00, 0, 0 };
    if (!calc_crc(id, buf, 2, &buf[2])) return;
    uint8_t rx[1];
    uint8_t len = sizeof(rx);
    transceive(id, buf, 4, rx, &len, 0);      /* card does not reply: timeout is normal */
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
esp_err_t rfid_bus_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) return ESP_OK;   /* already set up by another module */
    return err;
}

esp_err_t rfid_init(rfid_id_t id, int cs_pin, int rst_pin)
{
    /* Hardware reset pulse */
    gpio_reset_pin(rst_pin);
    gpio_set_direction(rst_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = cs_pin,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev, &s_dev[id]);
    if (err != ESP_OK) return err;

    reg_write(id, REG_COMMAND, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Timer: ~25 ms timeout so a missing card fails fast */
    reg_write(id, REG_TMODE, 0x80);
    reg_write(id, REG_TPRESCALER, 0xA9);
    reg_write(id, REG_TRELOAD_H, 0x03);
    reg_write(id, REG_TRELOAD_L, 0xE8);
    reg_write(id, REG_TXASK, 0x40);           /* 100% ASK modulation */
    reg_write(id, REG_MODE, 0x3D);            /* CRC preset 0x6363 */
    reg_set_bits(id, REG_TXCONTROL, 0x03);    /* antenna on */

    uint8_t v = reg_read(id, REG_VERSION);
    ESP_LOGI(TAG, "reader %d (CS %d) version register: 0x%02X", id, cs_pin, v);
    return (v == 0x00 || v == 0xFF) ? ESP_FAIL : ESP_OK;
}

bool rfid_read_uid(rfid_id_t id, char *uid_out, size_t len)
{
    uint8_t uid[4];
    if (!picc_request(id)) return false;
    if (!picc_select(id, uid)) return false;
    picc_halt(id);
    snprintf(uid_out, len, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    return true;
}