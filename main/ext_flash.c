/*
 * External SPI NOR flash (U2). See ext_flash.h.
 *
 * S25FL128L datasheet (Cypress 002-00124 Rev. *E) facts this relies on:
 *  - SOIC8: 1 CS#, 2 SO/IO1, 3 WP#/IO2, 4 VSS, 5 SI/IO0, 6 SCK,
 *    7 IO3/RESET#, 8 VCC; 2.7-3.6 V. No HOLD# function.
 *  - Factory state: array erased (FFh), SR1NV 00h (no block protection),
 *    CR1NV 00h (QUAD 0), CR2NV 60h (IO3_Reset off, 3-byte addresses).
 *  - READ 03h up to 50 MHz, PP 02h 256 bytes, SE 20h 4 KiB (max 250 ms),
 *    BE D8h 64 KiB (max 725 ms), RDSR1 05h, WREN 06h, RDID 9Fh.
 * The ESP-IDF generic chip driver uses exactly these commands, a 256-byte
 * page, 4 KiB sectors, 64 KiB blocks and longer time-outs (600 ms sector,
 * 4.1 s block), and takes the size from ID byte 3 (0x18 = 16 MiB).
 *
 * Single SPI for now: WP#/IO2 and IO3 are driven HIGH as plain GPIOs so
 * they can never protect or reset the chip.
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"

#include "board_pins.h"
#include "ext_flash.h"

#define EXT_FLASH_HOST      SPI2_HOST
#define EXT_FLASH_FREQ_MHZ  20

static esp_flash_t *chip;
static uint32_t chip_size;

esp_err_t ext_flash_init(void)
{
    /* WP#/IO2 and IO3/RESET# HIGH: inactive in single SPI mode. */
    gpio_set_level(PIN_NOR_WP, 1);
    gpio_set_level(PIN_NOR_HOLD, 1);
    const gpio_config_t io23 = {
        .pin_bit_mask = (1ULL << PIN_NOR_WP) | (1ULL << PIN_NOR_HOLD),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io23));

    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_NOR_MOSI,
        .miso_io_num = PIN_NOR_MISO,
        .sclk_io_num = PIN_NOR_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(EXT_FLASH_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        printf("ERROR: SPI bus for external flash: %s\n", esp_err_to_name(err));
        return err;
    }

    const esp_flash_spi_device_config_t dev = {
        .host_id = EXT_FLASH_HOST,
        .cs_io_num = PIN_NOR_CS,
        .io_mode = SPI_FLASH_SLOWRD,        /* READ 03h, single line */
        .freq_mhz = EXT_FLASH_FREQ_MHZ,
    };
    err = spi_bus_add_flash_device(&chip, &dev);
    if (err != ESP_OK) {
        printf("ERROR: add external flash device: %s\n", esp_err_to_name(err));
        return err;
    }

    err = esp_flash_init(chip);
    if (err != ESP_OK) {
        printf("ERROR: external flash not detected (%s)\n", esp_err_to_name(err));
        chip = NULL;
        return err;
    }

    uint32_t id = 0;
    esp_flash_read_id(chip, &id);
    esp_flash_get_size(chip, &chip_size);
    if (id == 0 || id == 0xffffff) {
        printf("ERROR: external flash not responding (ID %06lx)\n", (unsigned long)id);
        chip = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    printf("External flash detected\n");
    printf("JEDEC ID: %02lX %02lX %02lX%s\n", (unsigned long)(id >> 16),
           (unsigned long)((id >> 8) & 0xff), (unsigned long)(id & 0xff),
           id == EXT_FLASH_EXPECTED_ID ? " (Infineon/Cypress S25FL128L)"
                                       : " (NOT the expected S25FL128L 01 60 18)");
    printf("Flash size: %lu bytes\n", (unsigned long)chip_size);
    return ESP_OK;
}

bool ext_flash_ready(void)
{
    return chip != NULL;
}

uint32_t ext_flash_size(void)
{
    return chip ? chip_size : 0;
}

esp_err_t ext_flash_read(uint32_t addr, void *buf, size_t len)
{
    if (!chip) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_flash_read(chip, buf, addr, len);
}

esp_err_t ext_flash_write(uint32_t addr, const void *buf, size_t len)
{
    if (!chip) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_flash_write(chip, buf, addr, len);
}

esp_err_t ext_flash_erase(uint32_t addr, size_t len)
{
    if (!chip) {
        return ESP_ERR_INVALID_STATE;
    }
    if (addr % EXT_FLASH_SECTOR_SIZE || len % EXT_FLASH_SECTOR_SIZE ||
        addr + len > chip_size) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Uses 64 KiB block erase for aligned parts, 4 KiB sectors elsewhere. */
    return esp_flash_erase_region(chip, addr, len);
}

bool ext_flash_is_blank(uint32_t addr, size_t len)
{
    uint32_t buf[64];

    while (len) {
        size_t n = len < sizeof(buf) ? len : sizeof(buf);
        if (ext_flash_read(addr, buf, n) != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < n / 4; i++) {
            if (buf[i] != 0xffffffff) {
                return false;
            }
        }
        addr += n;
        len -= n;
    }
    return true;
}
