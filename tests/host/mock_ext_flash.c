/*
 * RAM model of the external NOR flash for host tests: program can only
 * clear bits (1 -> 0), erase works on whole 4 KiB sectors and sets 0xFF.
 * The capacity is configurable (16/32/64 MiB); every erase range is
 * recorded, accesses beyond the capacity are counted and refused, and
 * writes can be made to fail (power cut) after a number of calls.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext_flash.h"
#include "flash_layout.h"
#include "mock_ext_flash.h"

uint8_t *mock_flash;
uint32_t mock_capacity = 16u << 20;
mock_erase_t mock_erases[MOCK_MAX_ERASES];
int mock_erase_count;
int mock_program_violations;
int mock_out_of_range;
int mock_fail_writes_after = -1;

void mock_flash_reset(uint8_t fill)
{
    if (!mock_flash) {
        mock_flash = malloc(MOCK_FLASH_MAX);
    }
    memset(mock_flash, fill, MOCK_FLASH_MAX);
    mock_erase_count = 0;
    mock_program_violations = 0;
    mock_out_of_range = 0;
    mock_fail_writes_after = -1;
}

void mock_flash_setup(uint32_t capacity, uint8_t fill)
{
    mock_capacity = capacity;
    mock_flash_reset(fill);
}

static int in_range(uint32_t addr, size_t len)
{
    if ((uint64_t)addr + len > mock_capacity) {
        mock_out_of_range++;
        return 0;
    }
    return 1;
}

esp_err_t ext_flash_init(void) { return ESP_OK; }
bool ext_flash_ready(void) { return true; }
uint32_t ext_flash_size(void) { return mock_capacity; }

esp_err_t ext_flash_read(uint32_t addr, void *buf, size_t len)
{
    if (!in_range(addr, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(buf, mock_flash + addr, len);
    return ESP_OK;
}

esp_err_t ext_flash_write(uint32_t addr, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    if (!in_range(addr, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mock_fail_writes_after == 0) {
        return ESP_FAIL;
    }
    if (mock_fail_writes_after > 0) {
        mock_fail_writes_after--;
    }
    for (size_t i = 0; i < len; i++) {
        if (src[i] & ~mock_flash[addr + i]) {
            mock_program_violations++;
        }
        mock_flash[addr + i] &= src[i];
    }
    return ESP_OK;
}

esp_err_t ext_flash_erase(uint32_t addr, size_t len)
{
    if (addr % RF_SECTOR_SIZE || len % RF_SECTOR_SIZE || !in_range(addr, len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mock_erase_count < MOCK_MAX_ERASES) {
        mock_erases[mock_erase_count++] = (mock_erase_t){ addr, (uint32_t)len };
    }
    memset(mock_flash + addr, 0xff, len);
    return ESP_OK;
}

bool ext_flash_is_blank(uint32_t addr, size_t len)
{
    if (!in_range(addr, len)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (mock_flash[addr + i] != 0xff) {
            return false;
        }
    }
    return true;
}
