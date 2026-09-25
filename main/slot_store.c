/*
 * RadioFloppy slot store. See slot_store.h and docs/FLASH_LAYOUT.md.
 *
 * Catalog updates never touch the current copy: the new catalog
 * (generation + 1) goes to the other sector, is read back and only then
 * gets its commit word. The valid copy with the highest generation wins.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_rom_crc.h"

#include "ext_flash.h"
#include "legacy_catalog.h"
#include "slot_store.h"

#define CAT_MAGIC       0x4c534652  /* "RFSL" little endian */
#define CAT_VERSION     1
#define CAT_COMMIT      0x21544d43  /* "CMT!", last word of the sector */
#define COMMIT_OFFSET   (RF_SECTOR_SIZE - 4)
#define IO_CHUNK        4096

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;       /* 32 */
    uint32_t generation;
    uint16_t record_size;       /* 64 */
    uint16_t slot_count;        /* RF_SLOT_COUNT */
    uint32_t slot_base;         /* RF_SLOT_BASE */
    uint32_t slot_size;         /* RF_SLOT_SIZE */
    uint32_t crc32;             /* header + records, this field = 0 */
    uint8_t pad[4];
} cat_header_t;

typedef struct {
    cat_header_t hdr;
    slot_record_t slot[RF_SLOT_COUNT];
} catalog_t;

_Static_assert(sizeof(cat_header_t) == 32, "catalog header layout");
_Static_assert(sizeof(catalog_t) <= COMMIT_OFFSET, "catalog fits in a sector");

static catalog_t cat;               /* current catalog */
static catalog_t work;              /* new catalog / read-back buffer */
static slot_store_state_t state = SLOT_STORE_INVALID;
static uint32_t cat_addr;           /* sector holding the current catalog */
static int prepared_slot = -1;      /* slot prepared in this session */

uint32_t slot_store_crc32(uint32_t crc, const void *data, uint32_t len)
{
    return esp_rom_crc32_le(crc, data, len);
}

static uint32_t catalog_crc(const catalog_t *c)
{
    static const uint8_t zero[4] = { 0 };
    const uint8_t *p = (const uint8_t *)c;
    const size_t off = offsetof(cat_header_t, crc32);

    uint32_t crc = esp_rom_crc32_le(0, p, off);
    crc = esp_rom_crc32_le(crc, zero, sizeof(zero));
    return esp_rom_crc32_le(crc, p + off + 4, sizeof(*c) - off - 4);
}

static bool read_catalog(uint32_t addr, catalog_t *c)
{
    uint32_t commit = 0;

    if (ext_flash_read(addr, c, sizeof(*c)) != ESP_OK ||
        ext_flash_read(addr + COMMIT_OFFSET, &commit, sizeof(commit)) != ESP_OK) {
        return false;
    }
    return c->hdr.magic == CAT_MAGIC && c->hdr.version == CAT_VERSION &&
           c->hdr.header_size == sizeof(cat_header_t) &&
           c->hdr.record_size == sizeof(slot_record_t) &&
           c->hdr.slot_count == RF_SLOT_COUNT &&
           c->hdr.slot_base == RF_SLOT_BASE && c->hdr.slot_size == RF_SLOT_SIZE &&
           c->hdr.crc32 == catalog_crc(c) && commit == CAT_COMMIT;
}

slot_store_state_t slot_store_open(void)
{
    static catalog_t b;
    bool va = read_catalog(RF_SLOT_CAT_A, &cat);
    bool vb = read_catalog(RF_SLOT_CAT_B, &b);

    prepared_slot = -1;
    if (va || vb) {
        cat_addr = RF_SLOT_CAT_A;
        if (vb && (!va || b.hdr.generation > cat.hdr.generation)) {
            cat = b;
            cat_addr = RF_SLOT_CAT_B;
        }
        state = SLOT_STORE_VALID;
    } else if (ext_flash_is_blank(RF_SLOT_CAT_A, RF_SECTOR_SIZE) &&
               ext_flash_is_blank(RF_SLOT_CAT_B, RF_SECTOR_SIZE)) {
        state = SLOT_STORE_BLANK;
    } else {
        state = SLOT_STORE_INVALID;
    }
    return state;
}

slot_store_state_t slot_store_state(void)
{
    return state;
}

uint32_t slot_store_generation(void)
{
    return state == SLOT_STORE_VALID ? cat.hdr.generation : 0;
}

const slot_record_t *slot_store_record(int slot)
{
    if (state != SLOT_STORE_VALID || slot < 0 || slot >= RF_SLOT_COUNT) {
        return NULL;
    }
    return &cat.slot[slot];
}

bool slot_store_is_valid(int slot)
{
    const slot_record_t *r = slot_store_record(slot);

    return r && r->status == SLOT_VALID && r->size > 0 && r->size <= RF_IMAGE_MAX_SIZE &&
           r->format == SLOT_FMT_ST && memchr(r->name, 0, SLOT_NAME_LEN) != NULL;
}

void slot_record_title(const slot_record_t *r, char out[SLOT_TITLE_SIZE])
{
    size_t n = strnlen(r->name, SLOT_NAME_LEN - 1);

    memcpy(out, r->name, n);
    /* The continuation only counts after a full name field. */
    if (n == SLOT_NAME_LEN - 1 && (uint8_t)r->name_ext[0] != 0xff) {
        size_t e = strnlen(r->name_ext, SLOT_NAME_EXT_LEN - 1);
        memcpy(out + n, r->name_ext, e);
        n += e;
    }
    out[n] = 0;
}

/* Store a title of up to SLOT_TITLE_MAX characters in name + name_ext. */
static void set_title(slot_record_t *r, const char *title)
{
    size_t len = strnlen(title, SLOT_TITLE_MAX);
    size_t n = len < SLOT_NAME_LEN - 1 ? len : SLOT_NAME_LEN - 1;

    memset(r->name, 0, sizeof(r->name));
    memcpy(r->name, title, n);
    memset(r->name_ext, 0xff, sizeof(r->name_ext));
    if (len > n) {
        memset(r->name_ext, 0, sizeof(r->name_ext));
        memcpy(r->name_ext, title + n, len - n);
    }
}

int slot_store_find_name(const char *name)
{
    char title[SLOT_TITLE_SIZE];

    for (int i = 0; i < RF_SLOT_COUNT; i++) {
        if (slot_store_is_valid(i)) {
            slot_record_title(&cat.slot[i], title);
            if (strcmp(title, name) == 0) {
                return i;
            }
        }
    }
    return -1;
}

int slot_store_find_free(void)
{
    /* Prefer never-used slots, then deleted, then interrupted uploads. */
    static const uint8_t order[] = { SLOT_EMPTY, SLOT_DELETED, SLOT_BUILDING };

    if (state == SLOT_STORE_BLANK) {
        return 0;
    }
    for (size_t o = 0; o < sizeof(order); o++) {
        for (int i = 0; i < RF_SLOT_COUNT; i++) {
            const slot_record_t *r = slot_store_record(i);
            if (r && r->status == order[o]) {
                return i;
            }
        }
    }
    return -1;
}

/* Write c as the new catalog into the sector not holding the current one. */
static esp_err_t write_catalog(catalog_t *c)
{
    static catalog_t check;
    const uint32_t commit = CAT_COMMIT;
    uint32_t addr = (state == SLOT_STORE_VALID && cat_addr == RF_SLOT_CAT_A) ? RF_SLOT_CAT_B
                                                                            : RF_SLOT_CAT_A;
    esp_err_t err;

    if (state == SLOT_STORE_INVALID) {
        return ESP_ERR_INVALID_STATE;       /* never overwrite unknown data */
    }

    c->hdr.magic = CAT_MAGIC;
    c->hdr.version = CAT_VERSION;
    c->hdr.header_size = sizeof(cat_header_t);
    c->hdr.record_size = sizeof(slot_record_t);
    c->hdr.slot_count = RF_SLOT_COUNT;
    c->hdr.slot_base = RF_SLOT_BASE;
    c->hdr.slot_size = RF_SLOT_SIZE;
    c->hdr.generation = state == SLOT_STORE_VALID ? cat.hdr.generation + 1 : 1;
    c->hdr.crc32 = catalog_crc(c);

    if ((err = ext_flash_erase(addr, RF_SECTOR_SIZE)) != ESP_OK ||
        (err = ext_flash_write(addr, c, sizeof(*c))) != ESP_OK ||
        (err = ext_flash_read(addr, &check, sizeof(check))) != ESP_OK) {
        return err;
    }
    if (memcmp(&check, c, sizeof(check)) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    if ((err = ext_flash_write(addr + COMMIT_OFFSET, &commit, sizeof(commit))) != ESP_OK) {
        return err;
    }
    if (!read_catalog(addr, &check)) {
        return ESP_ERR_INVALID_CRC;
    }

    cat = *c;
    cat_addr = addr;
    state = SLOT_STORE_VALID;
    return ESP_OK;
}

/* Copy of the current catalog to modify, or an empty one on a blank store. */
static catalog_t *begin_update(void)
{
    if (state == SLOT_STORE_VALID) {
        work = cat;
    } else {
        memset(&work, 0xff, sizeof(work));
    }
    return &work;
}

esp_err_t slot_store_prepare(int slot, const char *name, uint8_t format,
                             uint32_t size, uint32_t crc32)
{
    if (state == SLOT_STORE_INVALID) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot < 0 || slot >= RF_SLOT_COUNT || size == 0 || size > RF_IMAGE_MAX_SIZE ||
        format != SLOT_FMT_ST || !name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    catalog_t *c = begin_update();
    if (c->slot[slot].status == SLOT_VALID) {
        return ESP_ERR_INVALID_STATE;       /* delete first; no in-place replace yet */
    }

    slot_record_t *r = &c->slot[slot];
    memset(r, 0xff, sizeof(*r));
    set_title(r, name);
    r->size = size;
    r->crc32 = crc32;
    r->format = format;
    r->status = SLOT_BUILDING;

    esp_err_t err = write_catalog(c);
    if (err != ESP_OK) {
        return err;
    }

    /* Erase only the sectors the image needs, inside this slot. */
    uint32_t start = rf_slot_start(slot);
    uint32_t len = rf_erase_len(size);
    if (len > RF_SLOT_SIZE || start + len > rf_slot_start(slot) + RF_SLOT_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((err = ext_flash_erase(start, len)) != ESP_OK) {
        return err;
    }
    prepared_slot = slot;
    return ESP_OK;
}

esp_err_t slot_store_write(int slot, uint32_t offset, const void *data, uint32_t len)
{
    const slot_record_t *r = slot_store_record(slot);

    if (slot != prepared_slot || !r || r->status != SLOT_BUILDING ||
        offset > r->size || len > r->size - offset) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Program via internal RAM: the source may be PSRAM or flash-mapped. */
    uint8_t *buf = heap_caps_malloc(IO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    for (uint32_t done = 0; done < len && err == ESP_OK; done += IO_CHUNK) {
        uint32_t n = len - done < IO_CHUNK ? len - done : IO_CHUNK;
        memcpy(buf, (const uint8_t *)data + done, n);
        err = ext_flash_write(rf_slot_start(slot) + offset + done, buf, n);
    }
    free(buf);
    return err;
}

/* CRC-32 of the first size bytes of a slot, read back from the flash. */
static esp_err_t slot_crc(int slot, uint32_t size, uint32_t *crc_out)
{
    uint8_t *buf = heap_caps_malloc(IO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint32_t crc = 0;
    esp_err_t err = buf ? ESP_OK : ESP_ERR_NO_MEM;

    for (uint32_t off = 0; off < size && err == ESP_OK; off += IO_CHUNK) {
        uint32_t n = size - off < IO_CHUNK ? size - off : IO_CHUNK;
        err = ext_flash_read(rf_slot_start(slot) + off, buf, n);
        crc = esp_rom_crc32_le(crc, buf, n);
    }
    free(buf);
    *crc_out = crc;
    return err;
}

esp_err_t slot_store_commit(int slot)
{
    const slot_record_t *r = slot_store_record(slot);
    uint32_t crc;

    if (slot != prepared_slot || !r || r->status != SLOT_BUILDING) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = slot_crc(slot, r->size, &crc);
    if (err != ESP_OK) {
        return err;
    }
    if (crc != r->crc32) {
        return ESP_ERR_INVALID_CRC;         /* stays BUILDING, never used */
    }

    catalog_t *c = begin_update();
    c->slot[slot].status = SLOT_VALID;
    err = write_catalog(c);
    if (err == ESP_OK) {
        prepared_slot = -1;
    }
    return err;
}

esp_err_t slot_store_delete(int slot)
{
    const slot_record_t *r = slot_store_record(slot);

    if (!r) {
        return ESP_ERR_INVALID_ARG;
    }
    if (r->status == SLOT_EMPTY || r->status == SLOT_DELETED) {
        return ESP_OK;
    }
    catalog_t *c = begin_update();
    c->slot[slot].status = SLOT_DELETED;
    if (prepared_slot == slot) {
        prepared_slot = -1;
    }
    return write_catalog(c);
}

esp_err_t slot_store_load(int slot, uint8_t *buf)
{
    if (!slot_store_is_valid(slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    const slot_record_t *r = &cat.slot[slot];
    esp_err_t err = ext_flash_read(rf_slot_start(slot), buf, r->size);
    if (err != ESP_OK) {
        return err;
    }
    return esp_rom_crc32_le(0, buf, r->size) == r->crc32 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t slot_store_migrate_legacy(int *migrated)
{
    *migrated = 0;
    if (state != SLOT_STORE_BLANK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!legacy_catalog_open()) {
        return ESP_ERR_NOT_FOUND;
    }

    catalog_t *c = begin_update();
    for (int i = 0; i < LEGACY_RECORDS; i++) {
        const legacy_record_t *l = legacy_catalog_record(i);
        if (!l || l->status != LEGACY_ST_VALID) {
            continue;
        }
        /* Only images whose bytes already sit at a slot start. */
        int slot = -1;
        for (int s = 0; s < RF_SLOT_COUNT; s++) {
            if (l->start == rf_slot_start(s)) {
                slot = s;
            }
        }
        uint32_t crc;
        if (slot < 0 || l->size == 0 || l->size > RF_IMAGE_MAX_SIZE || l->format != SLOT_FMT_ST ||
            c->slot[slot].status != SLOT_EMPTY ||
            slot_crc(slot, l->size, &crc) != ESP_OK || crc != l->crc32) {
            printf("Legacy image \"%.*s\" at 0x%06lx not migrated (not slot aligned, "
                   "too large or CRC mismatch)\n", LEGACY_NAME_LEN, l->name,
                   (unsigned long)l->start);
            continue;
        }
        slot_record_t *r = &c->slot[slot];
        char title[SLOT_TITLE_SIZE];
        memset(r, 0xff, sizeof(*r));
        memcpy(title, l->name, SLOT_NAME_LEN - 1);
        title[SLOT_NAME_LEN - 1] = 0;
        set_title(r, title);
        r->size = l->size;
        r->crc32 = l->crc32;
        r->format = SLOT_FMT_ST;
        r->status = SLOT_VALID;
        (*migrated)++;
    }
    if (*migrated == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return write_catalog(c);
}
