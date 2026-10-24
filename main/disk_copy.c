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

#ifdef DISK_COPY_NO_LOCK                /* host tests: single threaded */
void disk_copy_init(void) {}
void disk_copy_lock(void) {}
void disk_copy_unlock(void) {}
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t mutex;
void disk_copy_init(void)
{
    if (!mutex) {
        mutex = xSemaphoreCreateMutex();
    }
}
void disk_copy_lock(void) { xSemaphoreTake(mutex, portMAX_DELAY); }
void disk_copy_unlock(void) { xSemaphoreGive(mutex); }
#endif

#define COPY_MAGIC      0x59504352  /* "RCPY" little endian */
#define COPY_VERSION    3           /* 3: + pending, base_crc; 2: data at 0x020000 (1 at 0x011000) */
#define COMMIT_WORD     0x21544d43  /* "CMT!", last word of the header sector */
#define COMMIT_OFFSET   (RF_SECTOR_SIZE - 4)
#define SYNC_WORD       0x434e5953  /* "SYNC": a pending copy is on the card now */
#define SYNC_OFFSET     (RF_SECTOR_SIZE - 8)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t hfe;
    uint8_t pending;            /* newer than the card's file (until SYNC_WORD) */
    uint32_t size;
    uint32_t crc;               /* of the data */
    uint32_t base_crc;          /* pending: of the card's file it replaces */
    char path[SD_REL_MAX + 1];
    uint8_t reserved[3];
    uint32_t header_crc;        /* of everything before it */
} header_t;

_Static_assert(sizeof(header_t) <= SYNC_OFFSET, "header fits its sector");

static uint32_t w_size, w_done, w_crc;     /* the copy being written */
static bool w_patch;                        /* being patched: no running CRC */

static uint32_t header_crc(const header_t *h)
{
    return esp_rom_crc32_le(0, (const uint8_t *)h, offsetof(header_t, header_crc));
}

bool disk_copy_get(disk_copy_info_t *info)
{
    header_t h;
    uint32_t commit = 0, sync = 0;

    if (ext_flash_read(RF_COPY_HEADER, &h, sizeof(h)) != ESP_OK ||
        ext_flash_read(RF_COPY_HEADER + SYNC_OFFSET, &sync, sizeof(sync)) != ESP_OK ||
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
    info->pending = h.pending && sync != SYNC_WORD;
    info->base_crc = h.base_crc;
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
    w_patch = false;
    return ESP_OK;
}

/* Erase the 64 KiB block at off (the last one only as far as the copy
 * goes, in whole sectors). */
static esp_err_t erase_block(uint32_t off)
{
    uint32_t left = w_size - off;
    uint32_t erase = left >= RF_COPY_BLOCK ? RF_COPY_BLOCK
                   : (left + RF_SECTOR_SIZE - 1) / RF_SECTOR_SIZE * RF_SECTOR_SIZE;
    return ext_flash_erase(RF_COPY_DATA + off, erase);
}

esp_err_t disk_copy_write(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    esp_err_t err = ESP_OK;

    if (w_patch || len > w_size - w_done) {
        return ESP_ERR_INVALID_SIZE;
    }
    while (len && err == ESP_OK) {
        /* A block is erased when the first byte goes into it. */
        uint32_t in_block = w_done % RF_COPY_BLOCK;
        if (in_block == 0) {
            err = erase_block(w_done);
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

esp_err_t disk_copy_patch_begin(uint32_t size)
{
    disk_copy_info_t cur;

    if (!disk_copy_get(&cur) || cur.size != size) {
        return ESP_ERR_INVALID_STATE;       /* nothing to patch: write it whole */
    }
    esp_err_t err = disk_copy_clear();
    if (err != ESP_OK) {
        return err;
    }
    w_size = w_done = size;
    w_patch = true;
    return ESP_OK;
}

esp_err_t disk_copy_patch(uint32_t off, const void *data)
{
    if (!w_patch || off % RF_COPY_BLOCK || off >= w_size) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t n = w_size - off < RF_COPY_BLOCK ? w_size - off : RF_COPY_BLOCK;
    esp_err_t err = erase_block(off);
    return err == ESP_OK ? ext_flash_write(RF_COPY_DATA + off, data, n) : err;
}

esp_err_t disk_copy_commit(const disk_copy_info_t *info)
{
    static uint8_t buf[4096];
    static header_t h;
    esp_err_t err;

    if (w_done != w_size || info->size != w_size || (!w_patch && w_crc != info->crc) ||
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
    h.pending = info->pending;
    h.size = info->size;
    h.crc = info->crc;
    h.base_crc = info->pending ? info->base_crc : 0;
    memcpy(h.path, info->path, strlen(info->path) + 1);
    h.header_crc = header_crc(&h);
    const uint32_t commit = COMMIT_WORD;
    if ((err = ext_flash_write(RF_COPY_HEADER, &h, sizeof(h))) != ESP_OK ||
        (err = ext_flash_write(RF_COPY_HEADER + COMMIT_OFFSET, &commit, sizeof(commit))) != ESP_OK) {
        return err;
    }
    w_patch = false;
    return ESP_OK;
}

esp_err_t disk_copy_mark_synced(void)
{
    const uint32_t sync = SYNC_WORD;
    return ext_flash_write(RF_COPY_HEADER + SYNC_OFFSET, &sync, sizeof(sync));
}
