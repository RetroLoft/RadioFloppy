/*
 * Host tests for the block-based image library.
 *   tests/host/run.sh
 * Uses the real main/image_store.c on a RAM model of the NOR flash
 * (mock_ext_flash.c) with 16, 32 and 64 MiB capacity.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "ext_flash.h"
#include "flash_layout.h"
#include "image_store.h"
#include "mock_ext_flash.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MIB (1024u * 1024u)
#define KIB 1024u

static uint8_t *test_image(uint32_t size, uint8_t seed)
{
    uint8_t *d = malloc(size);
    uint32_t x = 0x12345678u ^ seed;
    for (uint32_t i = 0; i < size; i++) {
        x = x * 1103515245u + 12345u;
        d[i] = (uint8_t)(x >> 16);
    }
    return d;
}

/* Fresh flash of `capacity`, opened (a blank flash is initialised). */
static void fresh(uint32_t capacity)
{
    mock_flash_setup(capacity, 0xff);
    CHECK(image_store_open(capacity) == STORE_VALID);
}

static uint16_t save_new(uint32_t size, uint8_t seed, const char *name)
{
    uint8_t *d = test_image(size, seed);
    uint16_t id = 0;
    esp_err_t err = image_store_save(0, name, IMG_FMT_ST, d, size, &id);
    CHECK(err == ESP_OK);
    free(d);
    return err == ESP_OK ? id : 0;
}

/* The whole image read through the block list equals the test data. */
static int image_matches(uint16_t id, uint32_t size, uint8_t seed)
{
    image_record_t r;
    if (!image_store_get(id, &r) || r.original_size != size) {
        return 0;
    }
    uint8_t *want = test_image(size, seed);
    uint8_t *got = malloc(size);
    int ok = image_store_load(id, got) == ESP_OK && memcmp(want, got, size) == 0;
    free(want);
    free(got);
    return ok;
}

static void test_geometry(void)
{
    rf_geometry_t g;

    CHECK(rf_geometry_for(16 * MIB, &g) && g.block_size == 64 * KIB && g.data_blocks == 255 &&
          g.max_blocks == 24);
    CHECK(rf_geometry_for(32 * MIB, &g) && g.block_size == 128 * KIB && g.data_blocks == 255 &&
          g.max_blocks == 12);
    CHECK(rf_geometry_for(64 * MIB, &g) && g.block_size == 256 * KIB && g.data_blocks == 255 &&
          g.max_blocks == 6);
    CHECK(rf_geometry_for(128 * MIB, &g) && g.block_size == 512 * KIB && g.max_blocks == 3);
    CHECK(rf_geometry_for(8 * MIB, &g) && g.block_size == 64 * KIB && g.data_blocks == 127);
    CHECK(!rf_geometry_for(64 * KIB, &g));          /* no room for data */
    for (uint32_t cap = 1 * MIB; cap <= 1024u * MIB; cap *= 2) {
        CHECK(rf_geometry_for(cap, &g));
        CHECK(g.data_blocks <= 255);
        CHECK((uint64_t)(g.data_blocks + 1) * g.block_size <= cap);   /* inside the flash */
        CHECK(g.block_size >= 64 * KIB && (g.block_size & (g.block_size - 1)) == 0);
        CHECK(cap / g.block_size <= 256);
    }
}

static void test_contiguous_and_boundaries(void)
{
    fresh(16 * MIB);
    const rf_geometry_t *g = image_store_geometry();
    uint32_t size = 2 * g->block_size + g->block_size / 2;     /* 2.5 blocks */
    uint16_t id = save_new(size, 1, "Contiguous");
    image_record_t r;
    CHECK(image_store_get(id, &r));
    CHECK(r.block_count == 3 && r.blocks[0] == 1 && r.blocks[1] == 2 && r.blocks[2] == 3);
    CHECK(image_matches(id, size, 1));

    /* Read across a block boundary. */
    uint8_t *want = test_image(size, 1);
    uint8_t buf[64];
    CHECK(image_store_read(&r, g->block_size - 32, buf, 64) == ESP_OK &&
          memcmp(buf, want + g->block_size - 32, 64) == 0);
    CHECK(image_store_read(&r, size - 10, buf, 10) == ESP_OK &&
          memcmp(buf, want + size - 10, 10) == 0);
    CHECK(image_store_read(&r, size - 10, buf, 11) == ESP_ERR_INVALID_ARG);   /* past the end */
    free(want);

    /* Partly filled last block: only the sectors the image needs are
     * erased, and no erase leaves the chosen blocks. */
    int last = -1;
    for (int i = 0; i < mock_erase_count; i++) {
        if (mock_erases[i].addr == 3 * g->block_size) {
            last = i;
        }
        CHECK(mock_erases[i].addr < RF_BLOCK_SIZE_MIN ||
              (mock_erases[i].addr >= g->block_size &&
               mock_erases[i].addr + mock_erases[i].len <= 4 * g->block_size));
    }
    CHECK(last >= 0 && mock_erases[last].len == g->block_size / 2);
    CHECK(mock_program_violations == 0);
}

static void test_scattered(void)
{
    fresh(16 * MIB);
    const rf_geometry_t *g = image_store_geometry();
    uint16_t a = save_new(g->block_size, 10, "A");        /* block 1 */
    uint16_t b = save_new(g->block_size, 11, "B");        /* block 2 */
    uint16_t c = save_new(g->block_size, 12, "C");        /* block 3 */
    uint16_t d = save_new(g->block_size, 13, "D");        /* block 4 */
    CHECK(image_store_delete(b) == ESP_OK);
    CHECK(image_store_delete(d) == ESP_OK);

    /* Freed blocks 2 and 4 are reused: a non-contiguous image. */
    uint32_t size = g->block_size + 1000;
    uint16_t e = save_new(size, 14, "E");
    image_record_t r;
    CHECK(image_store_get(e, &r) && r.block_count == 2 && r.blocks[0] == 2 && r.blocks[1] == 4);
    CHECK(image_matches(e, size, 14));
    CHECK(image_matches(a, g->block_size, 10));
    CHECK(image_matches(c, g->block_size, 12));

    /* Ids are never reused; the usage follows the blocks. */
    CHECK(e != b && e != d);
    store_usage_t u;
    image_store_usage(&u);
    CHECK(u.images == 3 && u.blocks_used == 4 && u.blocks_total == 255 && u.blocks_free == 251);
    CHECK(u.used_percent == 2);

    /* After a restart everything is the same. */
    CHECK(image_store_open(16 * MIB) == STORE_VALID);
    CHECK(image_matches(e, size, 14));
    CHECK(mock_program_violations == 0 && mock_out_of_range == 0);
}

static void test_interrupted_upload(void)
{
    fresh(16 * MIB);
    const rf_geometry_t *g = image_store_geometry();
    uint16_t a = save_new(g->block_size, 20, "Keep");
    uint32_t gen = image_store_generation();

    /* Power cut while the image data is written (2nd data write fails). */
    uint8_t *d = test_image(3 * g->block_size, 21);
    uint16_t id = 0;
    mock_fail_writes_after = 1;
    CHECK(image_store_save(0, "Broken", IMG_FMT_ST, d, 3 * g->block_size, &id) != ESP_OK);
    mock_fail_writes_after = -1;
    CHECK(image_store_open(16 * MIB) == STORE_VALID);       /* as after the restart */
    CHECK(image_store_generation() == gen);
    image_record_t list[8];
    CHECK(image_store_list(list, 8) == 1 && list[0].id == a);
    store_usage_t u;
    image_store_usage(&u);
    CHECK(u.blocks_used == 1);                               /* its blocks are free */

    /* Power cut during the catalog update: 3 data writes + catalog body
     * succeed, the commit word does not. The previous catalog stays. */
    mock_fail_writes_after = 4;
    CHECK(image_store_save(0, "Broken", IMG_FMT_ST, d, 3 * g->block_size, &id) != ESP_OK);
    mock_fail_writes_after = -1;
    CHECK(image_store_open(16 * MIB) == STORE_VALID);
    CHECK(image_store_generation() == gen);
    CHECK(image_store_list(list, 8) == 1);

    /* The same save works afterwards and reuses the same blocks. */
    CHECK(image_store_save(0, "Works", IMG_FMT_ST, d, 3 * g->block_size, &id) == ESP_OK);
    CHECK(image_matches(id, 3 * g->block_size, 21));
    image_record_t r;
    CHECK(image_store_get(id, &r) && r.blocks[0] == 2);

    /* A damaged newest catalog copy: the previous generation is used. */
    uint32_t newest = image_store_generation();
    uint32_t addr = (newest % 2) ? RF_CAT_A : RF_CAT_B;     /* gen 1 went to copy A */
    mock_flash[addr + 100] ^= 0x01;
    CHECK(image_store_open(16 * MIB) == STORE_VALID);
    CHECK(image_store_generation() == newest - 1);
    CHECK(!image_store_get(id, &r));
    free(d);
}

static void fill_until_free(uint32_t free_blocks)
{
    const rf_geometry_t *g = image_store_geometry();
    store_usage_t u;
    int n = 0;
    for (image_store_usage(&u); u.blocks_free > free_blocks; image_store_usage(&u)) {
        uint32_t max = g->max_blocks < 20 ? g->max_blocks : 20;
        uint32_t blocks = u.blocks_free - free_blocks > max ? max : u.blocks_free - free_blocks;
        char name[16];
        snprintf(name, sizeof(name), "Fill %d", n++);
        save_new(blocks * g->block_size, (uint8_t)n, name);
    }
}

static void test_full_flash(void)
{
    fresh(16 * MIB);
    const rf_geometry_t *g = image_store_geometry();
    uint16_t low = save_new(2 * g->block_size, 29, "Low");     /* blocks 1, 2 */
    uint16_t x = save_new(3 * g->block_size, 30, "Replace me"); /* blocks 3, 4, 5 */
    fill_until_free(1);
    store_usage_t u;
    image_store_usage(&u);
    CHECK(u.blocks_free == 1);

    /* Not enough free blocks for a new 2-block image: refused, nothing changes. */
    uint32_t gen = image_store_generation();
    uint8_t *d = test_image(2 * g->block_size, 31);
    uint16_t id = 0;
    CHECK(!image_store_fits(2 * g->block_size, 0));
    CHECK(image_store_save(0, "Too big", IMG_FMT_ST, d, 2 * g->block_size, &id) == ESP_ERR_NO_MEM);
    CHECK(image_store_generation() == gen);
    CHECK(image_matches(x, 3 * g->block_size, 30));
    free(d);

    /* Replacing X (3 blocks) with a 4-block image works: 3 old + 1 free. */
    CHECK(image_store_delete(low) == ESP_OK);   /* free: 1, 2 and one high block */
    image_record_t old;
    CHECK(image_store_get(x, &old));
    uint32_t size = 3 * g->block_size + 5000;
    d = test_image(size, 32);
    CHECK(image_store_fits(size, x));
    CHECK(image_store_save(x, "Replaced", IMG_FMT_ST, d, size, &id) == ESP_OK && id == x);
    image_record_t r;
    CHECK(image_store_get(x, &r));
    CHECK(r.block_count == 4 && r.sequence == old.sequence);
    CHECK(old.blocks[0] == 3);
    CHECK(r.blocks[0] == old.blocks[0] && r.blocks[1] == old.blocks[1] &&
          r.blocks[2] == old.blocks[2] && r.blocks[3] == 1);  /* old blocks first */
    CHECK(image_matches(x, size, 32));
    image_store_usage(&u);
    CHECK(u.blocks_free == 2);
    fill_until_free(0);
    image_store_usage(&u);
    CHECK(u.blocks_free == 0 && u.used_percent == 100);
    free(d);

    /* Too big even with the old blocks: refused before the old image is touched. */
    d = test_image(6 * g->block_size, 33);
    CHECK(!image_store_fits(6 * g->block_size, x));
    CHECK(image_store_save(x, "Nope", IMG_FMT_ST, d, 6 * g->block_size, &id) == ESP_ERR_NO_MEM);
    CHECK(image_matches(x, size, 32));
    free(d);

    /* Replacing fails half way (power cut): the record is INCOMPLETE,
     * never valid; the old image is lost, its blocks are free. */
    d = test_image(2 * g->block_size, 34);
    mock_fail_writes_after = 3;         /* catalog (2) + first data write */
    CHECK(image_store_save(x, "Half", IMG_FMT_ST, d, 2 * g->block_size, &id) != ESP_OK);
    mock_fail_writes_after = -1;
    CHECK(image_store_open(16 * MIB) == STORE_VALID);
    CHECK(image_store_get(x, &r) && r.status == IMG_INCOMPLETE && r.block_count == 0);
    CHECK(!image_store_is_valid(&r));
    image_store_usage(&u);
    CHECK(u.blocks_free == 4);
    /* ... and it can be replaced again. */
    CHECK(image_store_save(x, "Again", IMG_FMT_ST, d, 2 * g->block_size, &id) == ESP_OK);
    CHECK(image_matches(x, 2 * g->block_size, 34));
    free(d);
    CHECK(mock_program_violations == 0 && mock_out_of_range == 0);
}

static void test_sequence(void)
{
    fresh(16 * MIB);
    uint16_t a = save_new(100000, 40, "A");
    uint16_t b = save_new(200000, 41, "B");
    uint16_t c = save_new(300000, 42, "C");
    image_record_t before[3], list[3];
    CHECK(image_store_list(before, 3) == 3);
    CHECK(before[0].id == a && before[1].id == b && before[2].id == c);
    uint32_t flash_crc = esp_rom_crc32_le(0, mock_flash + RF_BLOCK_SIZE_MIN, 16 * MIB - RF_BLOCK_SIZE_MIN);

    CHECK(image_store_set_position(c, 1) == ESP_OK);
    CHECK(image_store_list(list, 3) == 3);
    CHECK(list[0].id == c && list[1].id == a && list[2].id == b);
    CHECK(list[0].sequence == 1 && list[1].sequence == 2 && list[2].sequence == 3);
    CHECK(image_store_set_position(a, 99) == ESP_OK);          /* clamped: last */
    CHECK(image_store_list(list, 3) == 3 && list[2].id == a);

    /* No image data moved: same blocks, same bytes in the data area. */
    for (int i = 0; i < 3; i++) {
        image_record_t r;
        CHECK(image_store_get(before[i].id, &r));
        CHECK(memcmp(r.blocks, before[i].blocks, sizeof(r.blocks)) == 0 && r.crc32 == before[i].crc32);
    }
    CHECK(esp_rom_crc32_le(0, mock_flash + RF_BLOCK_SIZE_MIN, 16 * MIB - RF_BLOCK_SIZE_MIN) == flash_crc);
    CHECK(image_matches(a, 100000, 40) && image_matches(b, 200000, 41) && image_matches(c, 300000, 42));

    /* New images go to the end. */
    uint16_t d = save_new(1000, 43, "D");
    CHECK(image_store_list(list, 3) == 3);
    image_record_t all[4];
    CHECK(image_store_list(all, 4) == 4 && all[3].id == d);
    CHECK(image_store_find_name("B") == b && image_store_find_name("Nope") == 0);
}

static void test_limits(void)
{
    fresh(16 * MIB);
    uint8_t *d = test_image(RF_MAX_IMAGE_SIZE + 1, 50);
    uint16_t id = 0;
    CHECK(image_store_save(0, "Huge", IMG_FMT_ST, d, RF_MAX_IMAGE_SIZE + 1, &id) == ESP_ERR_INVALID_SIZE);
    CHECK(!image_store_fits(RF_MAX_IMAGE_SIZE + 1, 0));
    CHECK(image_store_save(0, "Empty", IMG_FMT_ST, d, 0, &id) == ESP_ERR_INVALID_SIZE);
    /* Exactly 1.5 MiB: 24 blocks of 64 KiB. */
    CHECK(image_store_save(0, "Max", IMG_FMT_ST, d, RF_MAX_IMAGE_SIZE, &id) == ESP_OK);
    image_record_t r;
    CHECK(image_store_get(id, &r) && r.block_count == 24);
    uint8_t *got = malloc(RF_MAX_IMAGE_SIZE);
    CHECK(image_store_load(id, got) == ESP_OK && memcmp(got, d, RF_MAX_IMAGE_SIZE) == 0);
    free(got);
    free(d);
    CHECK(image_store_delete(12345) == ESP_ERR_NOT_FOUND);
    CHECK(image_store_set_position(12345, 1) == ESP_ERR_NOT_FOUND);
}

/* Change one record in the newest catalog copy and fix its CRC. */
static void tamper(void (*edit)(image_record_t *rec), int rec_index)
{
    uint32_t addr = (image_store_generation() % 2) ? RF_CAT_A : RF_CAT_B;
    uint8_t *c = mock_flash + addr;
    size_t cat_size = 64 + IMG_MAX_RECORDS * sizeof(image_record_t);
    edit((image_record_t *)(c + 64 + rec_index * sizeof(image_record_t)));
    memset(c + 12, 0, 4);
    uint32_t crc = esp_rom_crc32_le(0, c, cat_size);
    memcpy(c + 12, &crc, 4);
}

static void dup_block(image_record_t *r) { r->blocks[0] = 1; }
static void bad_block(image_record_t *r) { r->blocks[0] = 0; }
static void too_many(image_record_t *r) { r->block_count = 25; }
static void size_mismatch(image_record_t *r) { r->stored_size += 1; }
static void compressed(image_record_t *r) { r->storage_format = 1; }

static void test_validation(void)
{
    void (*edits[])(image_record_t *) = { dup_block, bad_block, too_many, size_mismatch, compressed };
    for (size_t i = 0; i < sizeof(edits) / sizeof(edits[0]); i++) {
        fresh(16 * MIB);
        save_new(70000, 60, "One");         /* record 0: blocks 1, 2 */
        save_new(70000, 61, "Two");         /* record 1: blocks 3, 4 */
        uint32_t gen = image_store_generation();
        tamper(edits[i], 1);
        /* The tampered copy is refused; the previous generation is used. */
        CHECK(image_store_open(16 * MIB) == STORE_VALID);
        CHECK(image_store_generation() == gen - 1);
    }

    /* Catalog made for 16 MiB, flash reports 32 MiB: not used, not touched. */
    fresh(16 * MIB);
    save_new(1000, 62, "Geo");
    mock_capacity = 32 * MIB;
    CHECK(image_store_open(32 * MIB) == STORE_INVALID);
    CHECK(image_store_save(0, "X", IMG_FMT_ST, (const uint8_t *)"x", 1, NULL) == ESP_ERR_INVALID_STATE);
    mock_capacity = 16 * MIB;
    CHECK(image_store_open(16 * MIB) == STORE_VALID);

    /* Old 20-slot format: recognised, never read as new, format on request. */
    mock_flash_setup(16 * MIB, 0xff);
    const uint32_t old = 0x4c534652;
    memcpy(mock_flash + RF_OLD_SLOT_CAT_A, &old, 4);
    CHECK(image_store_open(16 * MIB) == STORE_OLD_FORMAT);
    image_record_t list[1];
    CHECK(image_store_list(list, 1) == 0);
    CHECK(image_store_format() == ESP_OK);
    CHECK(image_store_state() == STORE_VALID);
    CHECK(image_store_open(16 * MIB) == STORE_VALID);
    CHECK(image_store_generation() == 1);

    /* Unknown data in block 0: left alone. */
    mock_flash_setup(16 * MIB, 0xff);
    mock_flash[0x100] = 0x42;
    CHECK(image_store_open(16 * MIB) == STORE_INVALID);
    CHECK(mock_flash[0x100] == 0x42);
}

static void test_capacity(uint32_t capacity)
{
    fresh(capacity);
    const rf_geometry_t *g = image_store_geometry();
    CHECK(g->data_blocks == 255);
    /* Fill the whole flash, including the last block. */
    fill_until_free(0);
    store_usage_t u;
    image_store_usage(&u);
    CHECK(u.blocks_free == 0 && u.used_percent == 100);
    image_record_t *list = malloc(IMG_MAX_RECORDS * sizeof(*list));
    int n = image_store_list(list, IMG_MAX_RECORDS);
    int last_block = 0;
    for (int i = 0; i < n; i++) {
        for (int b = 0; b < list[i].block_count; b++) {
            if (list[i].blocks[b] > last_block) {
                last_block = list[i].blocks[b];
            }
        }
    }
    CHECK(last_block == 255);
    CHECK((uint64_t)(last_block + 1) * g->block_size <= capacity);
    free(list);
    /* 1.5 MiB needs max_blocks blocks at this block size. */
    CHECK(image_store_blocks_needed(RF_MAX_IMAGE_SIZE) == g->max_blocks);
    CHECK(mock_out_of_range == 0 && mock_program_violations == 0);
}

int main(void)
{
    printf("geometry\n");               test_geometry();
    printf("contiguous, block boundaries, partial last block\n");
    test_contiguous_and_boundaries();
    printf("scattered blocks, add/delete/reuse\n");   test_scattered();
    printf("interrupted upload and catalog update\n"); test_interrupted_upload();
    printf("nearly full flash, replace\n");            test_full_flash();
    printf("sequence\n");               test_sequence();
    printf("size limits\n");            test_limits();
    printf("catalog validation, old format\n");       test_validation();
    printf("32 MiB\n");                 test_capacity(32 * MIB);
    printf("64 MiB\n");                 test_capacity(64 * MIB);
    printf(failures ? "FAILED (%d)\n" : "image store OK\n", failures);
    return failures != 0;
}
