/*
 * RadioFloppy image library. See image_store.h and docs/FLASH_LAYOUT.md.
 *
 * Catalog updates never touch the copy in use: the new catalog
 * (generation + 1) goes to the other copy, is read back and only then gets
 * its commit word. The valid copy with the highest generation wins, so a
 * power cut during an update leaves the previous catalog in place.
 *
 * Writers (save, delete, set_position, set_read_write, commit_blocks,
 * format) are serialised by the writer lock: the HTTP API and the flush of
 * written sectors may call them from different tasks. Readers take the
 * short store lock and get copies of records.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_rom_crc.h"

#include "ext_flash.h"
#include "image_store.h"
#include "img_codec.h"

#ifdef IMAGE_STORE_NO_LOCK              /* host tests: single threaded */
#define store_lock()
#define store_unlock()
#define writer_lock()
#define writer_unlock()
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t mutex;         /* short: readers vs. the catalog swap */
static SemaphoreHandle_t writer;        /* long: one catalog/flash writer at a time */
#define store_lock()    xSemaphoreTake(mutex, portMAX_DELAY)
#define store_unlock()  xSemaphoreGive(mutex)
#define writer_lock()   xSemaphoreTake(writer, portMAX_DELAY)
#define writer_unlock() xSemaphoreGive(writer)
#endif

#define CAT_MAGIC       0x4c494652  /* "RFIL" little endian */
#define CAT_VERSION     2           /* 1 was the 20-slot catalog ("RFSL") */
#define CAT_COMMIT      0x21544d43  /* "CMT!", last word of the copy */
#define COMMIT_OFFSET   (RF_CAT_SIZE - 4)

#define OLD_SLOT_MAGIC  0x4c534652  /* "RFSL": 20-slot catalog */
#define OLD_V1_MAGIC    0x54434652  /* "RFCT": first catalog */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;       /* sizeof(cat_header_t) */
    uint32_t generation;        /* +1 per update; highest valid copy wins */
    uint32_t crc32;             /* header + records, this field = 0 */
    uint32_t capacity;          /* geometry the catalog was made for */
    uint32_t block_size;
    uint16_t data_blocks;
    uint16_t record_size;       /* sizeof(image_record_t) */
    uint16_t record_count;      /* IMG_MAX_RECORDS */
    uint8_t max_blocks_per_image;
    uint8_t reserved0;
    uint32_t max_image_size;
    uint16_t next_id;           /* id for the next new image */
    uint8_t reserved[26];
} cat_header_t;

typedef struct __attribute__((packed)) {
    cat_header_t hdr;
    image_record_t rec[IMG_MAX_RECORDS];
} catalog_t;

_Static_assert(sizeof(cat_header_t) == 64, "catalog header layout");
_Static_assert(sizeof(catalog_t) <= COMMIT_OFFSET, "catalog fits its copy");

static const uint32_t copy_addr[2] = { RF_CAT_A, RF_CAT_B };

static catalog_t *cat;              /* catalog in use */
static catalog_t *work;             /* scratch for the next generation */
static int cat_copy = -1;           /* copy the catalog came from */
static store_state_t state = STORE_NO_FLASH;
static rf_geometry_t geo;

uint32_t image_store_crc32(uint32_t crc, const void *data, uint32_t len)
{
    return esp_rom_crc32_le(crc, data, len);
}

static uint32_t catalog_crc(const catalog_t *c)
{
    const size_t off = offsetof(cat_header_t, crc32);
    uint32_t crc = image_store_crc32(0, c, off);
    const uint32_t zero = 0;
    crc = image_store_crc32(crc, &zero, sizeof(zero));
    return image_store_crc32(crc, (const uint8_t *)c + off + 4, sizeof(*c) - off - 4);
}

/* ---- Validation ---------------------------------------------------------- */

/* Every rule of docs/FLASH_LAYOUT.md; a copy that breaks one is not used. */
static bool catalog_consistent(const catalog_t *c)
{
    const cat_header_t *h = &c->hdr;
    uint8_t owner[256] = { 0 };
    uint16_t ids[IMG_MAX_RECORDS];
    int n_ids = 0;

    if (h->header_size != sizeof(cat_header_t) || h->record_size != sizeof(image_record_t) ||
        h->record_count != IMG_MAX_RECORDS || h->capacity != geo.capacity ||
        h->block_size != geo.block_size || h->data_blocks != geo.data_blocks ||
        h->max_blocks_per_image != RF_MAX_BLOCKS_PER_IMAGE ||
        h->max_image_size != RF_MAX_IMAGE_SIZE || h->next_id == 0) {
        return false;
    }
    for (int i = 0; i < IMG_MAX_RECORDS; i++) {
        const image_record_t *r = &c->rec[i];
        if (r->status == IMG_FREE) {
            continue;
        }
        if ((r->status != IMG_VALID && r->status != IMG_INCOMPLETE) || r->id == 0) {
            return false;
        }
        for (int j = 0; j < n_ids; j++) {
            if (ids[j] == r->id) {
                return false;               /* duplicate id */
            }
        }
        ids[n_ids++] = r->id;
        if (r->status == IMG_INCOMPLETE) {
            if (r->block_count != 0) {
                return false;
            }
            continue;
        }
        bool raw = r->storage_format == IMG_STORE_RAW;
        if (r->original_size == 0 || r->original_size > RF_MAX_IMAGE_SIZE ||
            (!raw && r->storage_format != IMG_STORE_DEFLATE) ||
            (raw ? r->stored_size != r->original_size
                 : r->stored_size == 0 || r->stored_size > r->original_size) ||
            r->block_count == 0 || r->block_count > RF_MAX_BLOCKS_PER_IMAGE ||
            r->block_count > geo.max_blocks ||
            r->block_count != rf_blocks_for(&geo, r->stored_size)) {
            return false;
        }
        for (int b = 0; b < r->block_count; b++) {
            uint8_t blk = r->blocks[b];
            if (blk == 0 || blk > geo.data_blocks || owner[blk]) {
                return false;               /* out of range or used twice */
            }
            owner[blk] = 1;
        }
    }
    return true;
}

static bool read_copy(int copy, catalog_t *c)
{
    uint32_t commit = 0;

    if (ext_flash_read(copy_addr[copy], c, sizeof(*c)) != ESP_OK ||
        ext_flash_read(copy_addr[copy] + COMMIT_OFFSET, &commit, sizeof(commit)) != ESP_OK) {
        return false;
    }
    return c->hdr.magic == CAT_MAGIC && c->hdr.version == CAT_VERSION &&
           commit == CAT_COMMIT && c->hdr.crc32 == catalog_crc(c) && catalog_consistent(c);
}

static uint32_t read_u32(uint32_t addr)
{
    uint32_t v = 0xffffffff;
    ext_flash_read(addr, &v, sizeof(v));
    return v;
}

/* ---- Catalog updates ------------------------------------------------------- */

/* Write `work` as the next generation into the other copy, then use it. */
static esp_err_t commit_work(void)
{
    static uint8_t check[1024];
    int target = cat_copy == 0 ? 1 : 0;
    uint32_t addr = copy_addr[target];
    const uint32_t commit = CAT_COMMIT;
    esp_err_t err;

    work->hdr.generation = cat->hdr.generation + 1;
    work->hdr.crc32 = catalog_crc(work);
    if ((err = ext_flash_erase(addr, RF_CAT_SIZE)) != ESP_OK ||
        (err = ext_flash_write(addr, work, sizeof(*work))) != ESP_OK) {
        return err;
    }
    for (uint32_t off = 0; off < sizeof(*work); off += sizeof(check)) {
        uint32_t n = sizeof(*work) - off < sizeof(check) ? sizeof(*work) - off : sizeof(check);
        if ((err = ext_flash_read(addr + off, check, n)) != ESP_OK) {
            return err;
        }
        if (memcmp(check, (const uint8_t *)work + off, n) != 0) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    if ((err = ext_flash_write(addr + COMMIT_OFFSET, &commit, sizeof(commit))) != ESP_OK) {
        return err;
    }
    store_lock();
    catalog_t *t = cat;
    cat = work;
    work = t;
    cat_copy = target;
    store_unlock();
    return ESP_OK;
}

static void begin_update(void)
{
    memcpy(work, cat, sizeof(*work));
}

static void empty_catalog(catalog_t *c, uint32_t generation)
{
    memset(c, 0xff, sizeof(*c));
    memset(&c->hdr, 0, sizeof(c->hdr));
    c->hdr.magic = CAT_MAGIC;
    c->hdr.version = CAT_VERSION;
    c->hdr.header_size = sizeof(cat_header_t);
    c->hdr.generation = generation;
    c->hdr.capacity = geo.capacity;
    c->hdr.block_size = geo.block_size;
    c->hdr.data_blocks = geo.data_blocks;
    c->hdr.record_size = sizeof(image_record_t);
    c->hdr.record_count = IMG_MAX_RECORDS;
    c->hdr.max_blocks_per_image = RF_MAX_BLOCKS_PER_IMAGE;
    c->hdr.max_image_size = RF_MAX_IMAGE_SIZE;
    c->hdr.next_id = 1;
}

static esp_err_t format_unlocked(void)
{
    if (!cat || state == STORE_NO_FLASH) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Both copies erased (this also removes old-format catalogs), then an
     * empty catalog as generation 1 in copy A. */
    esp_err_t err = ext_flash_erase(RF_CAT_A, 2 * RF_CAT_SIZE);
    if (err != ESP_OK) {
        return err;
    }
    empty_catalog(cat, 0);
    cat_copy = 1;                       /* so the next write goes to copy A */
    empty_catalog(work, 0);
    err = commit_work();
    store_lock();
    state = err == ESP_OK ? STORE_VALID : STORE_INVALID;
    store_unlock();
    return err;
}

/* ---- Opening --------------------------------------------------------------- */

store_state_t image_store_open(uint32_t capacity)
{
#ifndef IMAGE_STORE_NO_LOCK
    if (!mutex) {
        mutex = xSemaphoreCreateMutex();
        writer = xSemaphoreCreateMutex();
    }
#endif
    state = STORE_NO_FLASH;
    cat_copy = -1;
    if (!ext_flash_ready() || capacity == 0) {
        return state;
    }
    if (!cat) {
        cat = heap_caps_malloc(sizeof(catalog_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        work = heap_caps_malloc(sizeof(catalog_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!cat || !work) {
            return state;
        }
    }
    if (!rf_geometry_for(capacity, &geo)) {
        state = STORE_INVALID;
        return state;
    }

    bool ok[2];
    ok[0] = read_copy(0, cat);
    ok[1] = read_copy(1, work);
    if (ok[0] || ok[1]) {
        int best = ok[0] && (!ok[1] || cat->hdr.generation >= work->hdr.generation) ? 0 : 1;
        if (best == 1) {
            catalog_t *t = cat;
            cat = work;
            work = t;
        }
        cat_copy = best;
        state = STORE_VALID;
        return state;
    }

    if (read_u32(RF_OLD_SLOT_CAT_A) == OLD_SLOT_MAGIC ||
        read_u32(RF_OLD_SLOT_CAT_B) == OLD_SLOT_MAGIC ||
        read_u32(0x000000) == OLD_V1_MAGIC || read_u32(0x001000) == OLD_V1_MAGIC) {
        state = STORE_OLD_FORMAT;
        empty_catalog(cat, 0);          /* nothing listed until formatted */
        return state;
    }
    if (ext_flash_is_blank(RF_CAT_A, 2 * RF_CAT_SIZE)) {
        state = STORE_BLANK;
        empty_catalog(cat, 0);
        cat_copy = 1;                   /* first catalog goes to copy A */
        empty_catalog(work, 0);
        if (commit_work() == ESP_OK) {
            state = STORE_VALID;
        }
        return state;
    }
    state = STORE_INVALID;
    empty_catalog(cat, 0);
    return state;
}

store_state_t image_store_state(void)
{
    return state;
}

const char *image_store_state_name(store_state_t st)
{
    switch (st) {
    case STORE_VALID:       return "valid";
    case STORE_BLANK:       return "blank";
    case STORE_OLD_FORMAT:  return "old_format";
    case STORE_INVALID:     return "invalid";
    default:                return "no_flash";
    }
}

const rf_geometry_t *image_store_geometry(void)
{
    return &geo;
}

uint32_t image_store_generation(void)
{
    return cat && state == STORE_VALID ? cat->hdr.generation : 0;
}

/* ---- Queries --------------------------------------------------------------- */

static int find_index(uint16_t id)
{
    for (int i = 0; id && i < IMG_MAX_RECORDS; i++) {
        if (cat->rec[i].status != IMG_FREE && cat->rec[i].id == id) {
            return i;
        }
    }
    return -1;
}

static bool before(const image_record_t *a, const image_record_t *b)
{
    return a->sequence < b->sequence || (a->sequence == b->sequence && a->id < b->id);
}

/* Record indexes sorted by sequence (caller holds the lock or is the writer). */
static int sorted_indexes(const catalog_t *c, int idx[IMG_MAX_RECORDS])
{
    int n = 0;

    for (int i = 0; i < IMG_MAX_RECORDS; i++) {
        if (c->rec[i].status == IMG_FREE) {
            continue;
        }
        int j = n++;
        while (j > 0 && before(&c->rec[i], &c->rec[idx[j - 1]])) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = i;
    }
    return n;
}

int image_store_list(image_record_t *out, int max)
{
    static int idx[IMG_MAX_RECORDS];
    int n = 0;

    if (state != STORE_VALID) {
        return 0;
    }
    store_lock();
    int count = sorted_indexes(cat, idx);
    for (int i = 0; i < count && n < max; i++) {
        out[n++] = cat->rec[idx[i]];
    }
    store_unlock();
    return n;
}

bool image_store_get(uint16_t id, image_record_t *out)
{
    if (state != STORE_VALID) {
        return false;
    }
    store_lock();
    int i = find_index(id);
    if (i >= 0) {
        *out = cat->rec[i];
    }
    store_unlock();
    return i >= 0;
}

bool image_store_is_valid(const image_record_t *r)
{
    return r && r->status == IMG_VALID;
}

void image_store_title(const image_record_t *r, char out[IMG_TITLE_SIZE])
{
    size_t n = 0;
    while (n < IMG_NAME_LEN && r->name[n] && r->name[n] != (char)0xff) {
        n++;
    }
    memcpy(out, r->name, n);
    out[n] = 0;
}

uint16_t image_store_find_name(const char *name)
{
    static int idx[IMG_MAX_RECORDS];
    char title[IMG_TITLE_SIZE];
    uint16_t id = 0;

    if (state != STORE_VALID) {
        return 0;
    }
    store_lock();
    int count = sorted_indexes(cat, idx);
    for (int i = 0; i < count && !id; i++) {
        const image_record_t *r = &cat->rec[idx[i]];
        image_store_title(r, title);
        if (r->status == IMG_VALID && strcmp(title, name) == 0) {
            id = r->id;
        }
    }
    store_unlock();
    return id;
}

/* Blocks owned by valid records, except `skip` (record index or -1). */
static void used_map(const catalog_t *c, int skip, uint8_t used[256])
{
    memset(used, 0, 256);
    used[0] = 1;                        /* catalog and settings */
    for (int b = geo.data_blocks + 1; b < 256; b++) {
        used[b] = 1;                    /* beyond the flash */
    }
    for (int i = 0; i < IMG_MAX_RECORDS; i++) {
        const image_record_t *r = &c->rec[i];
        if (i != skip && r->status == IMG_VALID) {
            for (int b = 0; b < r->block_count; b++) {
                used[r->blocks[b]] = 1;
            }
        }
    }
}

void image_store_usage(store_usage_t *u)
{
    uint8_t used[256];

    memset(u, 0, sizeof(*u));
    if (state != STORE_VALID) {
        return;
    }
    store_lock();
    used_map(cat, -1, used);
    for (int i = 0; i < IMG_MAX_RECORDS; i++) {
        u->images += cat->rec[i].status == IMG_VALID;
    }
    store_unlock();
    u->blocks_total = geo.data_blocks;
    for (int b = 1; b <= geo.data_blocks; b++) {
        u->blocks_used += used[b];
    }
    u->blocks_free = u->blocks_total - u->blocks_used;
    u->used_percent = u->blocks_total ?
        (uint8_t)((u->blocks_used * 100u + u->blocks_total / 2) / u->blocks_total) : 0;
}

uint32_t image_store_blocks_needed(uint32_t size)
{
    return rf_blocks_for(&geo, size);
}

/*
 * Blocks for an image of `need` blocks: the blocks of record `reuse`
 * first (replacing), then free blocks in ascending order. Returns the
 * number found (< need: does not fit).
 */
static int pick_blocks(int reuse, uint32_t need, uint8_t out[RF_MAX_BLOCKS_PER_IMAGE])
{
    uint8_t used[256];
    uint32_t n = 0;

    used_map(cat, reuse, used);
    if (reuse >= 0) {
        const image_record_t *r = &cat->rec[reuse];
        for (int b = 0; b < r->block_count && n < need; b++) {
            out[n++] = r->blocks[b];
            used[r->blocks[b]] = 1;
        }
    }
    for (int b = 1; b <= geo.data_blocks && n < need; b++) {
        if (!used[b]) {
            out[n++] = (uint8_t)b;
        }
    }
    return (int)n;
}

bool image_store_fits(uint32_t size, uint16_t replace_id)
{
    uint8_t blocks[RF_MAX_BLOCKS_PER_IMAGE];

    if (state != STORE_VALID || size == 0 || size > RF_MAX_IMAGE_SIZE) {
        return false;
    }
    int reuse = -1;
    if (replace_id) {
        reuse = find_index(replace_id);
        if (reuse < 0) {
            return false;
        }
    } else {
        bool record = false;
        for (int i = 0; i < IMG_MAX_RECORDS && !record; i++) {
            record = cat->rec[i].status == IMG_FREE;
        }
        if (!record) {
            return false;
        }
    }
    uint32_t need = rf_blocks_for(&geo, size);
    return need <= geo.max_blocks && pick_blocks(reuse, need, blocks) == (int)need;
}

/* ---- Reading ----------------------------------------------------------------- */

esp_err_t image_store_read(const image_record_t *r, uint32_t offset, void *buf, uint32_t len)
{
    uint8_t *dst = buf;

    if (!r || r->block_count == 0 || r->block_count > RF_MAX_BLOCKS_PER_IMAGE ||
        offset > r->stored_size || len > r->stored_size - offset) {
        return ESP_ERR_INVALID_ARG;
    }
    while (len) {
        uint32_t bi = offset / geo.block_size;
        uint32_t within = offset % geo.block_size;
        uint32_t n = geo.block_size - within;
        if (n > len) {
            n = len;
        }
        uint8_t blk = r->blocks[bi];
        if (bi >= r->block_count || blk == 0 || blk > geo.data_blocks) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = ext_flash_read(rf_block_addr(&geo, blk) + within, dst, n);
        if (err != ESP_OK) {
            return err;
        }
        dst += n;
        offset += n;
        len -= n;
    }
    return ESP_OK;
}

esp_err_t image_store_load(uint16_t id, uint8_t *buf)
{
    image_record_t r;

    if (!image_store_get(id, &r) || r.status != IMG_VALID) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err;
    if (r.storage_format == IMG_STORE_DEFLATE) {
        uint8_t *packed = heap_caps_malloc(r.stored_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!packed) {
            return ESP_ERR_NO_MEM;
        }
        err = image_store_read(&r, 0, packed, r.stored_size);
        if (err == ESP_OK) {
            err = img_inflate(packed, r.stored_size, buf, r.original_size);
        }
        free(packed);
    } else {
        err = image_store_read(&r, 0, buf, r.original_size);
    }
    if (err == ESP_OK && image_store_crc32(0, buf, r.original_size) != r.crc32) {
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

/* CRC-32 of the image as stored in the blocks (read back from flash). */
static esp_err_t readback_crc(const image_record_t *r, uint32_t *crc_out)
{
    static uint8_t chunk[4096];
    uint32_t crc = 0;

    for (uint32_t off = 0; off < r->stored_size; off += sizeof(chunk)) {
        uint32_t n = r->stored_size - off < sizeof(chunk) ? r->stored_size - off : sizeof(chunk);
        esp_err_t err = image_store_read(r, off, chunk, n);
        if (err != ESP_OK) {
            return err;
        }
        crc = image_store_crc32(crc, chunk, n);
    }
    *crc_out = crc;
    return ESP_OK;
}

/* ---- Writing ------------------------------------------------------------------ */

/*
 * Store an image whose bytes on the flash are stored[stored_size] in
 * storage format sfmt (RAW: the image itself; DEFLATE: compressed);
 * size and crc are those of the image itself.
 */
static esp_err_t store_unlocked(uint16_t replace_id, const char *name, uint8_t format,
                                const uint8_t *data, uint32_t stored_size, uint8_t sfmt,
                                uint32_t size, uint32_t image_crc, uint16_t *id_out)
{
    image_record_t rec;
    esp_err_t err;

    if (state != STORE_VALID) {
        return ESP_ERR_INVALID_STATE;
    }
    if (size == 0 || size > RF_MAX_IMAGE_SIZE || stored_size == 0 || stored_size > size) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t need = rf_blocks_for(&geo, stored_size);
    if (need > geo.max_blocks) {
        return ESP_ERR_INVALID_SIZE;
    }

    int idx = -1;
    if (replace_id) {
        idx = find_index(replace_id);
        if (idx < 0) {
            return ESP_ERR_NOT_FOUND;
        }
    } else {
        for (int i = 0; i < IMG_MAX_RECORDS && idx < 0; i++) {
            if (cat->rec[i].status == IMG_FREE) {
                idx = i;
            }
        }
        if (idx < 0) {
            return ESP_ERR_NO_MEM;          /* no free record */
        }
    }

    memset(&rec, 0, sizeof(rec));
    if (pick_blocks(replace_id ? idx : -1, need, rec.blocks) != (int)need) {
        return ESP_ERR_NO_MEM;              /* checked before the old image is touched */
    }

    /* Replacing: from here on the old image is gone; its blocks are free. */
    if (replace_id && cat->rec[idx].status == IMG_VALID) {
        begin_update();
        image_record_t *o = &work->rec[idx];
        o->status = IMG_INCOMPLETE;
        o->block_count = 0;
        o->original_size = o->stored_size = 0;
        o->crc32 = 0;
        memset(o->blocks, 0, sizeof(o->blocks));
        if ((err = commit_work()) != ESP_OK) {
            return err;
        }
    }

    /* Erase and program only the chosen blocks (the last one only as far
     * as the image goes). */
    for (uint32_t i = 0; i < need; i++) {
        uint32_t off = i * geo.block_size;
        uint32_t n = stored_size - off < geo.block_size ? stored_size - off : geo.block_size;
        uint32_t addr = rf_block_addr(&geo, rec.blocks[i]);
        uint32_t erase = (n + RF_SECTOR_SIZE - 1) / RF_SECTOR_SIZE * RF_SECTOR_SIZE;
        if ((err = ext_flash_erase(addr, erase)) != ESP_OK ||
            (err = ext_flash_write(addr, data + off, n)) != ESP_OK) {
            return err;
        }
    }

    uint32_t crc = image_store_crc32(0, data, stored_size);    /* of the stored bytes */
    uint32_t back = 0;
    rec.block_count = (uint8_t)need;
    rec.original_size = size;
    rec.stored_size = stored_size;
    if ((err = readback_crc(&rec, &back)) != ESP_OK) {
        return err;
    }
    if (back != crc) {
        return ESP_ERR_INVALID_CRC;
    }

    /* Verified: now the record becomes valid. */
    begin_update();
    image_record_t *w = &work->rec[idx];
    if (replace_id) {
        rec.id = w->id;
        rec.sequence = w->sequence;
    } else {
        /* Next unused id (after 65535 it wraps and skips ids in use). */
        uint16_t id = work->hdr.next_id;
        for (bool taken = true; taken;) {
            taken = false;
            for (int i = 0; i < IMG_MAX_RECORDS && !taken; i++) {
                taken = work->rec[i].status != IMG_FREE && work->rec[i].id == id;
            }
            if (taken && ++id == 0) {
                id = 1;
            }
        }
        rec.id = id;
        work->hdr.next_id = (uint16_t)(id + 1) ? (uint16_t)(id + 1) : 1;
        uint16_t seq = 0;
        for (int i = 0; i < IMG_MAX_RECORDS; i++) {
            if (work->rec[i].status != IMG_FREE && work->rec[i].sequence > seq) {
                seq = work->rec[i].sequence;
            }
        }
        rec.sequence = seq + 1;
    }
    rec.status = IMG_VALID;
    /* New uploads and replacements are READ_ONLY; READ_WRITE only when the
     * caller asks for it (later: a new blank disk) and the format allows it. */
    rec.format = (format & IMG_FMT_MASK) |
                 ((format & IMG_FLAG_READ_WRITE) && image_format_writable(format & IMG_FMT_MASK)
                  ? IMG_FLAG_READ_WRITE : 0);
    rec.storage_format = sfmt;
    rec.crc32 = image_crc;
    memset(rec.name, 0, sizeof(rec.name));
    memcpy(rec.name, name, strnlen(name, sizeof(rec.name)));   /* 52 chars: no NUL needed */
    *w = rec;
    if ((err = commit_work()) != ESP_OK) {
        return err;
    }
    if (id_out) {
        *id_out = rec.id;
    }
    return ESP_OK;
}

/* Compress data; the result is used only if it is smaller and inflates
 * back to exactly the image (checked here, before anything is stored). */
static uint8_t *pack(const uint8_t *data, uint32_t size, uint32_t crc, uint32_t *packed_size)
{
    uint8_t *packed = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = 0;
    uint32_t back = 0;
    const char *why = "no memory";

    if (packed) {
        n = img_deflate(data, size, packed, size);
        why = "not smaller";
        if (n && (img_inflate_crc(packed, n, size, &back) != ESP_OK || back != crc)) {
            n = 0;                      /* never store what does not come back */
            why = "check failed";
        }
    }
    if (!n) {
        printf("Image store: stored uncompressed (%s)\n", why);
        free(packed);
        return NULL;
    }
    *packed_size = (uint32_t)n;
    return packed;
}

static esp_err_t save_unlocked(uint16_t replace_id, const char *name, uint8_t format,
                               const uint8_t *data, uint32_t size, bool compress,
                               uint16_t *id_out)
{
    if (size == 0 || size > RF_MAX_IMAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t crc = image_store_crc32(0, data, size);
    uint32_t packed_size = 0;
    /* A disk that may be written stays RAW: written sectors are saved block by block. */
    uint8_t *packed = compress && !(format & IMG_FLAG_READ_WRITE) ?
                      pack(data, size, crc, &packed_size) : NULL;
    esp_err_t err = packed ?
        store_unlocked(replace_id, name, format, packed, packed_size, IMG_STORE_DEFLATE, size,
                       crc, id_out) :
        store_unlocked(replace_id, name, format, data, size, IMG_STORE_RAW, size, crc, id_out);
    free(packed);
    return err;
}

static esp_err_t delete_unlocked(uint16_t id)
{
    if (state != STORE_VALID) {
        return ESP_ERR_INVALID_STATE;
    }
    int idx = find_index(id);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    begin_update();
    memset(&work->rec[idx], 0xff, sizeof(work->rec[idx]));
    return commit_work();
}

static esp_err_t set_position_unlocked(uint16_t id, int position)
{
    static int idx[IMG_MAX_RECORDS];

    if (state != STORE_VALID) {
        return ESP_ERR_INVALID_STATE;
    }
    int me = find_index(id);
    if (me < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    int n = sorted_indexes(cat, idx);
    int from = 0;
    while (idx[from] != me) {
        from++;
    }
    int to = position < 1 ? 0 : position > n ? n - 1 : position - 1;
    if (from < to) {
        memmove(&idx[from], &idx[from + 1], (to - from) * sizeof(idx[0]));
    } else if (from > to) {
        memmove(&idx[to + 1], &idx[to], (from - to) * sizeof(idx[0]));
    }
    idx[to] = me;

    begin_update();
    bool changed = false;
    for (int i = 0; i < n; i++) {
        changed |= work->rec[idx[i]].sequence != i + 1;
        work->rec[idx[i]].sequence = (uint16_t)(i + 1);
    }
    return changed ? commit_work() : ESP_OK;
}

/* ---- Format and write setting ---------------------------------------------- */

static const struct {
    uint8_t format;
    const char *name;
    bool writable;              /* may be set to READ_WRITE */
} formats[] = {
    { IMG_FMT_ST,  "st",  true },
    { IMG_FMT_MSA, "msa", false },
    { IMG_FMT_STX, "stx", false },
    { IMG_FMT_IPF, "ipf", false },
    { IMG_FMT_HFE, "hfe", false },
    { IMG_FMT_ADF, "adf", false },  /* later: standard AmigaDOS disks */
    { IMG_FMT_IMG, "img", false },  /* later: raw DOS sector images */
};

bool image_format_writable(uint8_t format)
{
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        if (formats[i].format == format) {
            return formats[i].writable;
        }
    }
    return false;
}

const char *image_format_name(uint8_t format)
{
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        if (formats[i].format == format) {
            return formats[i].name;
        }
    }
    return "unknown";
}

/*
 * A compressed image becomes RAW (to be written): the image goes to free
 * blocks, is read back, and one catalog update switches blocks, storage
 * format and the new format byte. The compressed version stays valid
 * until then.
 */
static esp_err_t unpack_unlocked(int idx, uint8_t new_format)
{
    static uint8_t check[1024];
    image_record_t rec = cat->rec[idx];
    uint8_t used[256];
    esp_err_t err;

    uint32_t size = rec.original_size;
    uint32_t need = rf_blocks_for(&geo, size);
    uint8_t *img = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img) {
        return ESP_ERR_NO_MEM;
    }
    if ((err = image_store_load(rec.id, img)) != ESP_OK) {
        free(img);
        return err;
    }
    used_map(cat, -1, used);
    memset(rec.blocks, 0, sizeof(rec.blocks));
    uint32_t n = 0;
    for (int b = 1; b <= geo.data_blocks && n < need; b++) {
        if (!used[b]) {
            rec.blocks[n++] = (uint8_t)b;
        }
    }
    if (n < need) {
        free(img);
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < need && err == ESP_OK; i++) {
        uint32_t off = i * geo.block_size;
        uint32_t len = size - off < geo.block_size ? size - off : geo.block_size;
        uint32_t addr = rf_block_addr(&geo, rec.blocks[i]);
        uint32_t erase = (len + RF_SECTOR_SIZE - 1) / RF_SECTOR_SIZE * RF_SECTOR_SIZE;
        if ((err = ext_flash_erase(addr, erase)) == ESP_OK) {
            err = ext_flash_write(addr, img + off, len);
        }
        for (uint32_t o = 0; err == ESP_OK && o < len; o += sizeof(check)) {
            uint32_t k = len - o < sizeof(check) ? len - o : sizeof(check);
            if ((err = ext_flash_read(addr + o, check, k)) == ESP_OK &&
                memcmp(check, img + off + o, k) != 0) {
                err = ESP_ERR_INVALID_CRC;
            }
        }
    }
    free(img);
    if (err != ESP_OK) {
        return err;
    }
    begin_update();
    image_record_t *w = &work->rec[idx];
    w->storage_format = IMG_STORE_RAW;
    w->stored_size = size;
    w->block_count = (uint8_t)need;
    memcpy(w->blocks, rec.blocks, sizeof(w->blocks));
    w->format = new_format;
    return commit_work();
}

static esp_err_t set_read_write_unlocked(uint16_t id, bool read_write)
{
    if (state != STORE_VALID) {
        return ESP_ERR_INVALID_STATE;
    }
    int idx = find_index(id);
    if (idx < 0 || cat->rec[idx].status != IMG_VALID) {
        return ESP_ERR_NOT_FOUND;
    }
    const image_record_t *r = &cat->rec[idx];
    if (read_write && !image_format_writable(image_format(r))) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (image_read_write(r) == read_write) {
        return ESP_OK;                  /* unchanged: no flash write */
    }
    uint8_t format = image_format(r) | (read_write ? IMG_FLAG_READ_WRITE : 0);
    if (read_write && r->storage_format != IMG_STORE_RAW) {
        return unpack_unlocked(idx, format);
    }
    begin_update();
    work->rec[idx].format = format;
    return commit_work();
}

/* ---- Written sectors: copy-on-write of changed blocks ------------------------ */

static esp_err_t commit_blocks_unlocked(uint16_t id, uint32_t mask, const uint8_t *image,
                                        uint32_t size, uint32_t crc)
{
    static uint8_t check[1024];
    uint8_t used[256];
    uint8_t fresh[RF_MAX_BLOCKS_PER_IMAGE];
    esp_err_t err;

    if (state != STORE_VALID) {
        return ESP_ERR_INVALID_STATE;
    }
    int idx = find_index(id);
    if (idx < 0 || cat->rec[idx].status != IMG_VALID || cat->rec[idx].original_size != size) {
        return ESP_ERR_NOT_FOUND;
    }
    image_record_t rec = cat->rec[idx];
    if (rec.storage_format != IMG_STORE_RAW) {
        return ESP_ERR_NOT_SUPPORTED;   /* writable disks are always stored RAW */
    }
    if (mask == 0) {
        return ESP_OK;
    }
    if (mask >> rec.block_count) {
        return ESP_ERR_INVALID_ARG;
    }

    /* New blocks: free ones only; the old blocks stay valid until the commit. */
    used_map(cat, -1, used);
    int b = 1;
    for (int i = 0; i < rec.block_count; i++) {
        if (!(mask & (1u << i))) {
            continue;
        }
        while (b <= geo.data_blocks && used[b]) {
            b++;
        }
        if (b > geo.data_blocks) {
            return ESP_ERR_NO_MEM;
        }
        fresh[i] = (uint8_t)b;
        used[b] = 1;
    }

    /* Program and compare every new block. */
    for (int i = 0; i < rec.block_count; i++) {
        if (!(mask & (1u << i))) {
            continue;
        }
        uint32_t off = i * geo.block_size;
        uint32_t n = size - off < geo.block_size ? size - off : geo.block_size;
        uint32_t addr = rf_block_addr(&geo, fresh[i]);
        uint32_t erase = (n + RF_SECTOR_SIZE - 1) / RF_SECTOR_SIZE * RF_SECTOR_SIZE;
        if ((err = ext_flash_erase(addr, erase)) != ESP_OK ||
            (err = ext_flash_write(addr, image + off, n)) != ESP_OK) {
            return err;
        }
        for (uint32_t o = 0; o < n; o += sizeof(check)) {
            uint32_t k = n - o < sizeof(check) ? n - o : sizeof(check);
            if ((err = ext_flash_read(addr + o, check, k)) != ESP_OK) {
                return err;
            }
            if (memcmp(check, image + off + o, k) != 0) {
                return ESP_ERR_INVALID_CRC;
            }
        }
        rec.blocks[i] = fresh[i];
    }

    /* The whole image as it will be stored must have the new CRC. */
    uint32_t back = 0;
    if ((err = readback_crc(&rec, &back)) != ESP_OK) {
        return err;
    }
    if (back != crc) {
        return ESP_ERR_INVALID_CRC;
    }

    /* One catalog update switches all changed blocks at once. */
    begin_update();
    work->rec[idx].crc32 = crc;
    memcpy(work->rec[idx].blocks, rec.blocks, sizeof(rec.blocks));
    return commit_work();
}

/* ---- Public writers: one at a time -------------------------------------------- */

esp_err_t image_store_format(void)
{
    writer_lock();
    esp_err_t err = format_unlocked();
    writer_unlock();
    return err;
}

esp_err_t image_store_save(uint16_t replace_id, const char *name, uint8_t format,
                           const uint8_t *data, uint32_t size, uint16_t *id_out)
{
    return image_store_save_ex(replace_id, name, format, data, size, false, id_out);
}

esp_err_t image_store_save_ex(uint16_t replace_id, const char *name, uint8_t format,
                              const uint8_t *data, uint32_t size, bool compress, uint16_t *id_out)
{
    writer_lock();
    esp_err_t err = save_unlocked(replace_id, name, format, data, size, compress, id_out);
    writer_unlock();
    return err;
}

esp_err_t image_store_delete(uint16_t id)
{
    writer_lock();
    esp_err_t err = delete_unlocked(id);
    writer_unlock();
    return err;
}

esp_err_t image_store_set_position(uint16_t id, int position)
{
    writer_lock();
    esp_err_t err = set_position_unlocked(id, position);
    writer_unlock();
    return err;
}

esp_err_t image_store_set_read_write(uint16_t id, bool read_write)
{
    writer_lock();
    esp_err_t err = set_read_write_unlocked(id, read_write);
    writer_unlock();
    return err;
}

esp_err_t image_store_commit_blocks(uint16_t id, uint32_t mask, const uint8_t *image,
                                    uint32_t size, uint32_t crc)
{
    writer_lock();
    esp_err_t err = commit_blocks_unlocked(id, mask, image, size, crc);
    writer_unlock();
    return err;
}
