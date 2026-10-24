/*
 * Host tests for the copy of the last disk in the flash (main/disk_copy.c)
 * on the RAM model of the NOR flash: written in pieces, read back; a
 * power cut or damaged data leaves no copy, never a wrong one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "disk_copy.h"
#include "flash_layout.h"
#include "mock_ext_flash.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint8_t *data_of(uint32_t n, uint8_t seed)
{
    uint8_t *d = malloc(n);
    uint32_t x = 0x9e3779b9u ^ seed;
    for (uint32_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; d[i] = (uint8_t)(x >> 16); }
    return d;
}

static esp_err_t write_all(const uint8_t *d, uint32_t n)
{
    esp_err_t err = disk_copy_begin(n);
    for (uint32_t o = 0; o < n && err == ESP_OK;) {
        uint32_t k = (uint32_t)(rand() % 9000) + 1;
        if (k > n - o) k = n - o;
        err = disk_copy_write(d + o, k);
        o += k;
    }
    return err;
}

int main(void)
{
    mock_flash_setup(16u << 20, 0xff);
    disk_copy_info_t info, got;

    CHECK(!disk_copy_get(&got));                     /* blank flash: no copy */

    uint32_t n = 737280;
    uint8_t *d = data_of(n, 1);
    info = (disk_copy_info_t) { .size = n, .crc = esp_rom_crc32_le(0, d, n), .hfe = false };
    strcpy(info.path, "L/Leisure Suit Larry (1987)(Sierra).st");
    CHECK(write_all(d, n) == ESP_OK);
    CHECK(!disk_copy_get(&got));                     /* not valid before the commit */
    CHECK(disk_copy_commit(&info) == ESP_OK);
    CHECK(disk_copy_get(&got) && got.size == n && got.crc == info.crc && !got.hfe &&
          strcmp(got.path, info.path) == 0);
    uint8_t *back = malloc(n);
    CHECK(disk_copy_read(0, back, n) == ESP_OK && memcmp(back, d, n) == 0);
    CHECK(mock_program_violations == 0 && mock_out_of_range == 0);

    /* The largest copy (3 MiB, an HFE file) fits and ends inside 4 MiB. */
    uint32_t big = RF_COPY_MAX;
    uint8_t *h = data_of(big, 2);
    disk_copy_info_t hi = { .size = big, .crc = esp_rom_crc32_le(0, h, big), .hfe = true };
    strcpy(hi.path, "Klax.hfe");
    CHECK(write_all(h, big) == ESP_OK && disk_copy_commit(&hi) == ESP_OK);
    CHECK(disk_copy_get(&got) && got.hfe && got.size == big);
    CHECK(disk_copy_begin(big + 1) == ESP_ERR_INVALID_SIZE);
    CHECK(disk_copy_get(&got));                      /* a refused begin keeps the copy */

    /* Power cut while writing: afterwards there is no copy. */
    mock_fail_writes_after = 5;
    CHECK(write_all(d, n) != ESP_OK);
    mock_fail_writes_after = -1;
    CHECK(!disk_copy_get(&got));

    /* Wrong CRC or incomplete data: no commit, no copy. */
    CHECK(write_all(d, n) == ESP_OK);
    disk_copy_info_t bad = info;
    bad.crc ^= 1;
    CHECK(disk_copy_commit(&bad) == ESP_ERR_INVALID_STATE && !disk_copy_get(&got));
    CHECK(disk_copy_begin(n) == ESP_OK && disk_copy_write(d, n / 2) == ESP_OK);
    CHECK(disk_copy_commit(&info) == ESP_ERR_INVALID_STATE && !disk_copy_get(&got));
    /* Damaged on the flash after writing: the read-back check refuses it. */
    CHECK(write_all(d, n) == ESP_OK);
    mock_flash[RF_COPY_DATA + 1000] ^= 0x01;
    CHECK(disk_copy_commit(&info) == ESP_ERR_INVALID_CRC && !disk_copy_get(&got));

    /* A good copy, then cleared. */
    CHECK(write_all(d, n) == ESP_OK && disk_copy_commit(&info) == ESP_OK && disk_copy_get(&got));
    CHECK(disk_copy_clear() == ESP_OK && !disk_copy_get(&got));
    /* A damaged header is no copy. */
    CHECK(write_all(d, n) == ESP_OK && disk_copy_commit(&info) == ESP_OK);
    mock_flash[RF_COPY_HEADER + 20] ^= 0x01;
    CHECK(!disk_copy_get(&got));
    CHECK(mock_out_of_range == 0);

    free(d); free(h); free(back);
    printf(failures ? "FAILED (%d)\n" : "disk copy OK\n", failures);
    return failures != 0;
}
