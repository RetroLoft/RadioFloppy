/*
 * External SPI NOR flash (U2) on SPI2 (FSPI IO_MUX pins), single SPI.
 *
 * Fitted part: Infineon/Cypress S25FL128L (JEDEC 01 60 18, 16 MiB,
 * 4 KiB sectors, 64 KiB blocks, 256-byte pages). ESP-IDF drives it with
 * its generic chip driver (same opcodes and sizes, see ext_flash.c).
 *
 * Every access goes through the functions below, which always pass our
 * own esp_flash_t: a NULL chip would address the ESP32 firmware flash.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define EXT_FLASH_SECTOR_SIZE   4096
#define EXT_FLASH_EXPECTED_ID   0x016018    /* S25FL128L */

/* Initialise the bus and the chip; prints the JEDEC ID and size. */
esp_err_t ext_flash_init(void);

bool ext_flash_ready(void);
uint32_t ext_flash_size(void);
uint32_t ext_flash_jedec_id(void);

esp_err_t ext_flash_read(uint32_t addr, void *buf, size_t len);
esp_err_t ext_flash_write(uint32_t addr, const void *buf, size_t len);

/* addr and len must be multiples of EXT_FLASH_SECTOR_SIZE. */
esp_err_t ext_flash_erase(uint32_t addr, size_t len);

/* true when [addr, addr+len) reads as all 0xFF (erased). */
bool ext_flash_is_blank(uint32_t addr, size_t len);
