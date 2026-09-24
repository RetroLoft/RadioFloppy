/*
 * RadioFloppy image store on the external SPI flash. See image_store.h and
 * docs/IMAGE_STORE.md.
 *
 * A catalog update never modifies the current copy: the new catalog (with
 * generation + 1) is written to the other 4 KiB sector, verified, and only
 * then gets its commit word. At start-up the valid copy with the highest
 * generation wins, so a power loss during an update leaves the previous
 * catalog in force.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_rom_crc.h"

#include "ext_flash.h"
#include "image_store.h"

#define COMMIT_OFFSET   (RF_ALIGN - 4)
#define CHUNK           4096

/* Catalog buffers are ~3.9 KiB each: static, never on the task stack. */
static rf_catalog_t catalog;
static rf_catalog_t scratch;        /* read-back / new catalog under construction */
static rf_catalog_t check_buf;
static bool have_catalog;
static uint32_t catalog_addr;       /* sector holding the current catalog */

uint32_t image_store_crc32(const uint8_t *data, uint32_t size)
{
    /* Standard CRC-32 (IEEE 802.3, as zlib crc32()). */
    return esp_rom_crc32_le(0, data, size);
}

/* CRC over header + records with the crc32 field counted as 0. */
static uint32_t catalog_crc(const rf_catalog_t *cat)
{
    static const uint8_t zero[4] = { 0 };
    const uint8_t *p = (const uint8_t *)cat;
    const size_t off = offsetof(rf_cat_header_t, crc32);

    uint32_t crc = esp_rom_crc32_le(0, p, off);
    crc = esp_rom_crc32_le(crc, zero, sizeof(zero));
    return esp_rom_crc32_le(crc, p + off + 4, sizeof(*cat) - off - 4);
}

/* Read and validate one catalog copy. */
static bool read_catalog(uint32_t addr, rf_catalog_t *cat)
{
    uint32_t commit = 0;

    if (ext_flash_read(addr, cat, sizeof(*cat)) != ESP_OK ||
        ext_flash_read(addr + COMMIT_OFFSET, &commit, sizeof(commit)) != ESP_OK) {
        return false;
    }
    return cat->hdr.magic == RF_CAT_MAGIC &&
           cat->hdr.version == RF_CAT_VERSION &&
           cat->hdr.header_size == sizeof(rf_cat_header_t) &&
           cat->hdr.record_size == sizeof(rf_record_t) &&
           cat->hdr.record_count == RF_CAT_RECORDS &&
           cat->hdr.crc32 == catalog_crc(cat) &&
           commit == RF_CAT_COMMIT;
}

esp_err_t image_store_open(void)
{
    rf_catalog_t *a = &scratch, *b = &check_buf;
    bool va = read_catalog(RF_CAT_ADDR_A, a);
    bool vb = read_catalog(RF_CAT_ADDR_B, b);

    have_catalog = va || vb;
    if (!have_catalog) {
        return ESP_ERR_NOT_FOUND;
    }
    if (va && (!vb || a->hdr.generation >= b->hdr.generation)) {
        catalog = *a;
        catalog_addr = RF_CAT_ADDR_A;
    } else {
        catalog = *b;
        catalog_addr = RF_CAT_ADDR_B;
    }
    return ESP_OK;
}

const rf_catalog_t *image_store_catalog(void)
{
    return have_catalog ? &catalog : NULL;
}

const rf_record_t *image_store_find(const char *name)
{
    if (!have_catalog) {
        return NULL;
    }
    for (int i = 0; i < RF_CAT_RECORDS; i++) {
        const rf_record_t *r = &catalog.rec[i];
        if (r->status == RF_ST_VALID && strncmp(r->name, name, RF_NAME_LEN) == 0) {
            return r;
        }
    }
    return NULL;
}

/* Write cat as the new catalog into the sector not holding the current one. */
static esp_err_t write_catalog(rf_catalog_t *cat)
{
    uint32_t addr = (have_catalog && catalog_addr == RF_CAT_ADDR_A) ? RF_CAT_ADDR_B
                                                                    : RF_CAT_ADDR_A;
    const uint32_t commit = RF_CAT_COMMIT;
    rf_catalog_t *check = &check_buf;
    esp_err_t err;

    cat->hdr.magic = RF_CAT_MAGIC;
    cat->hdr.version = RF_CAT_VERSION;
    cat->hdr.header_size = sizeof(rf_cat_header_t);
    cat->hdr.record_size = sizeof(rf_record_t);
    cat->hdr.record_count = RF_CAT_RECORDS;
    cat->hdr.generation = have_catalog ? catalog.hdr.generation + 1 : 1;
    cat->hdr.crc32 = catalog_crc(cat);

    if ((err = ext_flash_erase(addr, RF_ALIGN)) != ESP_OK ||
        (err = ext_flash_write(addr, cat, sizeof(*cat))) != ESP_OK ||
        (err = ext_flash_read(addr, check, sizeof(*check))) != ESP_OK) {
        return err;
    }
    if (memcmp(check, cat, sizeof(*check)) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    /* Commit word last: only now does this copy count as valid. */
    if ((err = ext_flash_write(addr + COMMIT_OFFSET, &commit, sizeof(commit))) != ESP_OK) {
        return err;
    }
    if (!read_catalog(addr, check)) {
        return ESP_ERR_INVALID_CRC;
    }

    catalog = *cat;
    catalog_addr = addr;
    have_catalog = true;
    printf("Catalog written: copy %c, generation %lu\n", addr == RF_CAT_ADDR_A ? 'A' : 'B',
           (unsigned long)cat->hdr.generation);
    return ESP_OK;
}

esp_err_t image_store_add(const char *name, uint8_t format,
                          const uint8_t *data, uint32_t size)
{
    rf_catalog_t *cat = &scratch;
    int slot = -1;
    uint32_t start = RF_IMAGE_BASE;
    uint32_t reserved = (size + RF_ALIGN - 1) / RF_ALIGN * RF_ALIGN;
    esp_err_t err;

    if (have_catalog) {
        *cat = catalog;
    } else {
        /* No catalog: the store must be blank, never destroy unknown data. */
        if (!ext_flash_is_blank(RF_CAT_ADDR_A, 2 * RF_ALIGN)) {
            printf("ERROR: catalog sectors contain unknown data - not overwriting\n");
            return ESP_ERR_INVALID_STATE;
        }
        memset(cat, 0xff, sizeof(*cat));
    }

    /* Next free space: after every used, building or deleted area. */
    for (int i = 0; i < RF_CAT_RECORDS; i++) {
        const rf_record_t *r = &cat->rec[i];
        if (r->status == RF_ST_EMPTY) {
            if (slot < 0) {
                slot = i;
            }
            continue;
        }
        if (r->start + r->reserved > start) {
            start = r->start + r->reserved;
        }
    }
    if (slot < 0) {
        printf("ERROR: catalog full\n");
        return ESP_ERR_NO_MEM;
    }
    if (start + reserved > ext_flash_size()) {
        printf("ERROR: not enough space on the external flash\n");
        return ESP_ERR_NO_MEM;
    }
    if (!have_catalog && !ext_flash_is_blank(start, reserved)) {
        printf("ERROR: image area 0x%06lx contains unknown data - not overwriting\n",
               (unsigned long)start);
        return ESP_ERR_INVALID_STATE;
    }

    rf_record_t *r = &cat->rec[slot];
    memset(r, 0xff, sizeof(*r));
    memset(r->name, 0, sizeof(r->name));
    strncpy(r->name, name, RF_NAME_LEN - 1);
    r->start = start;
    r->size = size;
    r->reserved = reserved;
    r->crc32 = image_store_crc32(data, size);
    r->format = format;
    r->status = RF_ST_BUILDING;

    printf("Storing \"%s\": %lu bytes at 0x%06lx, reserved 0x%06lx, next free 0x%06lx\n",
           name, (unsigned long)size, (unsigned long)start, (unsigned long)reserved,
           (unsigned long)(start + reserved));
    if ((err = write_catalog(cat)) != ESP_OK) {
        printf("ERROR: catalog write failed (%s)\n", esp_err_to_name(err));
        return err;
    }

    /* Erase only this image's own sectors. */
    if ((err = ext_flash_erase(start, reserved)) != ESP_OK) {
        printf("ERROR: erase failed (%s)\n", esp_err_to_name(err));
        return err;
    }
    if (!ext_flash_is_blank(start, reserved)) {
        printf("ERROR: image area not blank after erase\n");
        return ESP_FAIL;
    }

    /* Program via an internal RAM buffer, then read back and compare. */
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t off = 0; off < size && err == ESP_OK; off += CHUNK) {
        uint32_t n = size - off < CHUNK ? size - off : CHUNK;
        memcpy(buf, data + off, n);
        err = ext_flash_write(start + off, buf, n);
    }
    uint32_t mismatch = UINT32_MAX;
    for (uint32_t off = 0; off < size && err == ESP_OK; off += CHUNK) {
        uint32_t n = size - off < CHUNK ? size - off : CHUNK;
        err = ext_flash_read(start + off, buf, n);
        if (err == ESP_OK && memcmp(buf, data + off, n) != 0) {
            for (uint32_t i = 0; i < n; i++) {
                if (buf[i] != data[off + i]) {
                    mismatch = off + i;
                    break;
                }
            }
            break;
        }
    }
    free(buf);
    if (err != ESP_OK || mismatch != UINT32_MAX) {
        printf("ERROR: programming/verification failed (%s, first mismatch at %ld)\n",
               esp_err_to_name(err), (long)(mismatch == UINT32_MAX ? -1 : (long)mismatch));
        return err != ESP_OK ? err : ESP_ERR_INVALID_CRC;
    }
    printf("Readback verification: %lu bytes identical\n", (unsigned long)size);

    r->status = RF_ST_VALID;
    if ((err = write_catalog(cat)) != ESP_OK) {
        printf("ERROR: catalog write failed (%s)\n", esp_err_to_name(err));
    }
    return err;
}

esp_err_t image_store_load(const rf_record_t *rec, uint8_t *buf)
{
    esp_err_t err = ext_flash_read(rec->start, buf, rec->size);

    if (err != ESP_OK) {
        return err;
    }
    return image_store_crc32(buf, rec->size) == rec->crc32 ? ESP_OK : ESP_ERR_INVALID_CRC;
}
