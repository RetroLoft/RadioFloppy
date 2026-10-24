/*
 * The copy of the last disk from the SD card in the external flash. See
 * disk_copy.h.
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "esp_rom_crc.h"

#include "disk_copy.h"
#include "ext_flash.h"
#include "flash_layout.h"

#define COPY_MAGIC      0x59504352  /* "RCPY" little endian */
#define COPY_VERSION    2           /* 2: data at 0x020000 (1 had it at 0x011000) */
#define COMMIT_WORD     0x21544d43  /* "CMT!", last word of the header sector */
#define COMMIT_OFFSET   (RF_SECTOR_SIZE - 4)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t hfe;
    uint32_t size;
    uint32_t crc;               /* of the data */
    char path[SD_REL_MAX + 1];
    uint8_t reserved[3];
    uint32_t header_crc;        /* of everything before it */
} header_t;

_Static_assert(sizeof(header_t) <= COMMIT_OFFSET, "header fits its sector");

static uint32_t w_size, w_done, w_crc;     /* the copy being written */

static uint32_t header_crc(const header_t *h)
{
    return esp_rom_crc32_le(0, (const uint8_t *)h, offsetof(header_t, header_crc));
}

bool disk_copy_get(disk_copy_info_t *info)
{
    header_t h;
    uint32_t commit = 0;

    if (ext_flash_read(RF_COPY_HEADER, &h, sizeof(h)) != ESP_OK ||
        ext_flash_read(RF_COPY_HEADER + COMMIT_OFFSET, &commit, sizeof(commit)) != ESP_OK) {
        return false;
    }
    if (h.magic != COPY_MAGIC || h.version != COPY_VERSION || commit != COMMIT_WORD ||
        h.header_crc != header_crc(&h) || h.size == 0 || h.size > RF_COPY_MAX ||
        memchr(h.path, 0, sizeof(h.path)) == NULL) {
        return false;
    }
    info->size = h.size;
    info->crc = h.crc;
    info->hfe = h.hfe != 0;
    memcpy(info->path, h.path, sizeof(info->path));
    return true;
}

esp_err_t disk_copy_read(uint32_t off, void *buf, uint32_t len)
{
    if (off > RF_COPY_MAX || len > RF_COPY_MAX - off) {
        return ESP_ERR_INVALID_ARG;
    }
    return ext_flash_read(RF_COPY_DATA + off, buf, len);
}

esp_err_t disk_copy_clear(void)
{
    return ext_flash_erase(RF_COPY_HEADER, RF_SECTOR_SIZE);
}

esp_err_t disk_copy_begin(uint32_t size)
{
    if (size == 0 || size > RF_COPY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = disk_copy_clear();      /* from here on there is no valid copy */
    if (err != ESP_OK) {
        return err;
    }
    w_size = size;
    w_done = 0;
    w_crc = 0;
    return ESP_OK;
}

esp_err_t disk_copy_write(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    esp_err_t err = ESP_OK;

    if (len > w_size - w_done) {
        return ESP_ERR_INVALID_SIZE;
    }
    while (len && err == ESP_OK) {
        /* A 64 KiB block is erased when the first byte goes into it (the
         * last one only as far as the copy goes, in whole sectors). */
        uint32_t in_block = w_done % RF_COPY_BLOCK;
        if (in_block == 0) {
            uint32_t left = w_size - w_done;
            uint32_t erase = left >= RF_COPY_BLOCK ? RF_COPY_BLOCK
                           : (left + RF_SECTOR_SIZE - 1) / RF_SECTOR_SIZE * RF_SECTOR_SIZE;
            err = ext_flash_erase(RF_COPY_DATA + w_done, erase);
        }
        uint32_t n = RF_COPY_BLOCK - in_block < len ? RF_COPY_BLOCK - in_block : len;
        if (err == ESP_OK) {
            err = ext_flash_write(RF_COPY_DATA + w_done, p, n);
        }
        w_crc = esp_rom_crc32_le(w_crc, p, n);
        w_done += n;
        p += n;
        len -= n;
    }
    return err;
}

esp_err_t disk_copy_commit(const disk_copy_info_t *info)
{
    static uint8_t buf[4096];
    static header_t h;
    esp_err_t err;

    if (w_done != w_size || info->size != w_size || w_crc != info->crc ||
        strlen(info->path) > SD_REL_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    /* What is on the flash must be what was written. */
    uint32_t c = 0;
    for (uint32_t off = 0; off < w_size; off += sizeof(buf)) {
        uint32_t n = w_size - off < sizeof(buf) ? w_size - off : sizeof(buf);
        if ((err = ext_flash_read(RF_COPY_DATA + off, buf, n)) != ESP_OK) {
            return err;
        }
        c = esp_rom_crc32_le(c, buf, n);
    }
    if (c != info->crc) {
        return ESP_ERR_INVALID_CRC;
    }
    memset(&h, 0, sizeof(h));
    h.magic = COPY_MAGIC;
    h.version = COPY_VERSION;
    h.hfe = info->hfe;
    h.size = info->size;
    h.crc = info->crc;
    memcpy(h.path, info->path, strlen(info->path) + 1);
    h.header_crc = header_crc(&h);
    const uint32_t commit = COMMIT_WORD;
    if ((err = ext_flash_write(RF_COPY_HEADER, &h, sizeof(h))) != ESP_OK ||
        (err = ext_flash_write(RF_COPY_HEADER + COMMIT_OFFSET, &commit, sizeof(commit))) != ESP_OK) {
        return err;
    }
    return ESP_OK;
}
