/*
 * RAM model of the 16 MiB external NOR flash for host tests: program can
 * only clear bits (1 -> 0), erase works on whole 4 KiB sectors and sets
 * 0xFF. Every erase range is recorded so tests can check its bounds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext_flash.h"
#include "flash_layout.h"
#include "mock_ext_flash.h"

uint8_t mock_flash[RF_FLASH_SIZE];
mock_erase_t mock_erases[MOCK_MAX_ERASES];
int mock_erase_count;
int mock_program_violations;       /* attempts to program a 0 bit back to 1 */

void mock_flash_reset(uint8_t fill)
{
    memset(mock_flash, fill, sizeof(mock_flash));
    mock_erase_count = 0;
    mock_program_violations = 0;
}

esp_err_t ext_flash_init(void) { return ESP_OK; }
bool ext_flash_ready(void) { return true; }
uint32_t ext_flash_size(void) { return RF_FLASH_SIZE; }

esp_err_t ext_flash_read(uint32_t addr, void *buf, size_t len)
{
    if (addr + len > RF_FLASH_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(buf, mock_flash + addr, len);
    return ESP_OK;
}

esp_err_t ext_flash_write(uint32_t addr, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    if (addr + len > RF_FLASH_SIZE) {
        return ESP_ERR_INVALID_ARG;
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
    if (addr % RF_SECTOR_SIZE || len % RF_SECTOR_SIZE || addr + len > RF_FLASH_SIZE) {
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
    for (size_t i = 0; i < len; i++) {
        if (mock_flash[addr + i] != 0xff) {
            return false;
        }
    }
    return true;
}
