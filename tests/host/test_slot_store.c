/*
 * Host tests for the RadioFloppy flash layout and slot store.
 *   tests/host/run.sh
 * Uses the real main/slot_store.c and main/legacy_catalog.c on a RAM model
 * of the NOR flash (mock_ext_flash.c).
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "ext_flash.h"
#include "flash_layout.h"
#include "legacy_catalog.h"
#include "mock_ext_flash.h"
#include "slot_store.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t region_crc(uint32_t addr, uint32_t len)
{
    return esp_rom_crc32_le(0, mock_flash + addr, len);
}

static uint8_t *test_image(uint32_t size, uint8_t seed)
{
    uint8_t *d = malloc(size);
    for (uint32_t i = 0; i < size; i++) {
        d[i] = (uint8_t)(i * 7 + seed + (i >> 9));
    }
    return d;
}

/* Upload an image through the public API. */
static esp_err_t upload(int slot, const char *name, const uint8_t *d, uint32_t size)
{
    uint32_t crc = slot_store_crc32(0, d, size);
    esp_err_t e = slot_store_prepare(slot, name, SLOT_FMT_ST, size, crc);
    for (uint32_t off = 0; e == ESP_OK && off < size; off += 10000) {
        uint32_t n = size - off < 10000 ? size - off : 10000;
        e = slot_store_write(slot, off, d + off, n);
    }
    return e == ESP_OK ? slot_store_commit(slot) : e;
}

static void test_layout(void)
{
    printf("layout\n");
    CHECK(rf_slot_start(0) == 0x010000);
    CHECK(rf_slot_start(1) == 0x0D8000);
    CHECK(rf_slot_start(2) == 0x1A0000);
    CHECK(rf_slot_start(19) == 0xEE8000);
    CHECK(rf_slot_start(19) + RF_SLOT_SIZE - 1 == 0xFAFFFF);
    CHECK(RF_RESERVED_BASE == 0xFB0000 && RF_RESERVED_SIZE == 320 * 1024);
    CHECK(RF_RESERVED_BASE + RF_RESERVED_SIZE == RF_FLASH_SIZE);
    for (int i = 0; i < RF_SLOT_COUNT; i++) {
        uint32_t s = rf_slot_start(i), e = s + RF_SLOT_SIZE;
        CHECK(s % RF_SECTOR_SIZE == 0);
        CHECK(s >= RF_META_BASE + RF_META_SIZE);        /* after metadata */
        CHECK(e <= RF_RESERVED_BASE);                   /* before reserved tail */
        CHECK(e <= RF_FLASH_SIZE);
        if (i > 0) {
            CHECK(s == rf_slot_start(i - 1) + RF_SLOT_SIZE);   /* no gap, no overlap */
        }
    }
    CHECK(RF_SLOT_CAT_A >= RF_META_BASE && RF_SLOT_CAT_B + RF_SECTOR_SIZE <= RF_SLOT_BASE);
    CHECK(RF_SLOT_CAT_A != RF_LEGACY_CAT_A && RF_SLOT_CAT_A != RF_LEGACY_CAT_B);
    CHECK(rf_erase_len(368640) == 0x5A000);
    CHECK(rf_erase_len(1) == 0x1000 && rf_erase_len(RF_SLOT_SIZE) == RF_SLOT_SIZE);
}

static void test_blank_and_invalid(void)
{
    printf("blank / invalid store\n");
    mock_flash_reset(0xff);
    CHECK(slot_store_open() == SLOT_STORE_BLANK);
    CHECK(slot_store_find_free() == 0);

    mock_flash_reset(0xff);
    memset(mock_flash + RF_SLOT_CAT_B + 100, 0x12, 16);        /* unknown data */
    CHECK(slot_store_open() == SLOT_STORE_INVALID);
    uint8_t d[16] = { 1 };
    CHECK(slot_store_prepare(0, "X", SLOT_FMT_ST, 16, slot_store_crc32(0, d, 16)) != ESP_OK);
    CHECK(mock_erase_count == 0);                              /* nothing erased */
}

static void test_upload_and_bounds(void)
{
    printf("upload, erase bounds, untouched areas\n");
    mock_flash_reset(0xff);
    memset(mock_flash + RF_RESERVED_BASE, 0x5a, RF_RESERVED_SIZE);  /* sentinel */
    memset(mock_flash + rf_slot_start(1), 0x3c, RF_SLOT_SIZE);      /* neighbour */
    uint32_t reserved_crc = region_crc(RF_RESERVED_BASE, RF_RESERVED_SIZE);
    uint32_t slot2_crc = region_crc(rf_slot_start(1), RF_SLOT_SIZE);

    CHECK(slot_store_open() == SLOT_STORE_BLANK);
    uint8_t *cc = test_image(368640, 1);
    CHECK(upload(0, "Crystal Castles", cc, 368640) == ESP_OK);
    CHECK(slot_store_is_valid(0));
    CHECK(slot_store_find_name("Crystal Castles") == 0);
    CHECK(memcmp(mock_flash + rf_slot_start(0), cc, 368640) == 0);
    CHECK(mock_program_violations == 0);

    /* Only catalog sectors and the image's own sectors were erased. */
    for (int i = 0; i < mock_erase_count; i++) {
        mock_erase_t e = mock_erases[i];
        bool cat = (e.addr == RF_SLOT_CAT_A || e.addr == RF_SLOT_CAT_B) && e.len == RF_SECTOR_SIZE;
        bool img = e.addr == rf_slot_start(0) && e.len == 0x5A000;
        CHECK(cat || img);
    }
    CHECK(region_crc(RF_RESERVED_BASE, RF_RESERVED_SIZE) == reserved_crc);
    CHECK(region_crc(rf_slot_start(1), RF_SLOT_SIZE) == slot2_crc);
    CHECK(ext_flash_is_blank(RF_LEGACY_CAT_A, 2 * RF_SECTOR_SIZE));

    /* Reopen from flash. Load gives the image, exactly its real size. */
    CHECK(slot_store_open() == SLOT_STORE_VALID);
    uint8_t *buf = malloc(RF_SLOT_SIZE);
    CHECK(slot_store_load(0, buf) == ESP_OK);
    CHECK(memcmp(buf, cc, 368640) == 0);
    CHECK(slot_store_record(0)->size == 368640);

    /* Last slot, full 800 KiB: reserved tail still untouched. */
    uint8_t *big = test_image(RF_SLOT_SIZE, 9);
    CHECK(slot_store_find_free() == 1);
    CHECK(slot_store_delete(1) == ESP_OK);      /* EMPTY: no-op */
    CHECK(upload(19, "Full slot 20", big, RF_SLOT_SIZE) == ESP_OK);
    CHECK(region_crc(RF_RESERVED_BASE, RF_RESERVED_SIZE) == reserved_crc);
    CHECK(slot_store_load(19, buf) == ESP_OK && memcmp(buf, big, RF_SLOT_SIZE) == 0);

    /* Invalid sizes are refused. */
    CHECK(slot_store_prepare(2, "Too big", SLOT_FMT_ST, RF_SLOT_SIZE + 1, 0) == ESP_ERR_INVALID_ARG);
    CHECK(slot_store_prepare(2, "Empty", SLOT_FMT_ST, 0, 0) == ESP_ERR_INVALID_ARG);
    /* A valid slot cannot be overwritten in place. */
    CHECK(slot_store_prepare(0, "Other", SLOT_FMT_ST, 100, 0) == ESP_ERR_INVALID_STATE);
    /* Writing beyond the announced size is refused. */
    CHECK(slot_store_prepare(2, "Small", SLOT_FMT_ST, 100, 0) == ESP_OK);
    CHECK(slot_store_write(2, 90, cc, 11) == ESP_ERR_INVALID_STATE);
    free(cc); free(big); free(buf);
}

static void test_crc_and_interrupt(void)
{
    printf("verification failure, interrupted upload, delete, reuse\n");
    mock_flash_reset(0xff);
    slot_store_open();
    uint8_t *d = test_image(100000, 3);

    /* Data corrupted in flash before commit: stays BUILDING, never valid. */
    CHECK(slot_store_prepare(0, "Corrupt", SLOT_FMT_ST, 100000, slot_store_crc32(0, d, 100000)) == ESP_OK);
    CHECK(slot_store_write(0, 0, d, 100000) == ESP_OK);
    mock_flash[rf_slot_start(0) + 5000] ^= 0x01;
    CHECK(slot_store_commit(0) == ESP_ERR_INVALID_CRC);
    CHECK(!slot_store_is_valid(0) && slot_store_record(0)->status == SLOT_BUILDING);

    /* Interrupted upload (reset after half the data). */
    CHECK(slot_store_prepare(1, "Half", SLOT_FMT_ST, 100000, slot_store_crc32(0, d, 100000)) == ESP_OK);
    CHECK(slot_store_write(1, 0, d, 50000) == ESP_OK);
    CHECK(slot_store_open() == SLOT_STORE_VALID);          /* "reboot" */
    CHECK(slot_store_record(1)->status == SLOT_BUILDING);
    CHECK(slot_store_find_name("Half") == -1);
    CHECK(slot_store_commit(1) == ESP_ERR_INVALID_STATE);  /* not prepared this session */

    /* Good upload, delete, reuse with a smaller image. */
    CHECK(upload(2, "Game", d, 100000) == ESP_OK);
    CHECK(slot_store_delete(2) == ESP_OK);
    CHECK(slot_store_record(2)->status == SLOT_DELETED && !slot_store_is_valid(2));
    CHECK(memcmp(mock_flash + rf_slot_start(2), d, 100000) == 0);   /* bytes kept */
    int before = mock_erase_count;
    uint8_t *small = test_image(5000, 7);
    CHECK(upload(2, "Smaller", small, 5000) == ESP_OK);
    for (int i = before; i < mock_erase_count; i++) {
        if (mock_erases[i].addr >= RF_SLOT_BASE) {
            CHECK(mock_erases[i].addr == rf_slot_start(2) && mock_erases[i].len == 0x2000);
        }
    }
    /* Free slot order: empty first, then deleted, then interrupted. */
    CHECK(slot_store_find_free() == 3);
    free(d); free(small);
}

static void test_ab_fallback(void)
{
    printf("catalog A/B generations and fallback\n");
    mock_flash_reset(0xff);
    slot_store_open();
    uint8_t *d = test_image(4096, 5);
    CHECK(upload(0, "One", d, 4096) == ESP_OK);            /* gen 1 (A), gen 2 (B) */
    CHECK(upload(1, "Two", d, 4096) == ESP_OK);            /* gen 3 (A), gen 4 (B) */
    CHECK(slot_store_open() == SLOT_STORE_VALID && slot_store_generation() == 4);

    /* Newest copy (B) damaged: the older copy (A, gen 3) is used. */
    mock_flash[RF_SLOT_CAT_B + 200] ^= 0x80;
    CHECK(slot_store_open() == SLOT_STORE_VALID && slot_store_generation() == 3);
    CHECK(slot_store_record(1)->status == SLOT_BUILDING);  /* gen 3 = before commit */
    /* Missing commit word on B also invalidates it. */
    mock_flash_reset(0xff);
    slot_store_open();
    CHECK(upload(0, "One", d, 4096) == ESP_OK);
    memset(mock_flash + RF_SLOT_CAT_B + RF_SECTOR_SIZE - 4, 0xff, 4);
    CHECK(slot_store_open() == SLOT_STORE_VALID && slot_store_generation() == 1);
    free(d);
}

/* Write a legacy v1 catalog like the previous firmware did. */
static void write_legacy(const char *name, uint32_t start, const uint8_t *d, uint32_t size)
{
    struct __attribute__((packed)) {
        uint32_t magic; uint16_t version, header_size; uint32_t generation;
        uint16_t record_size, record_count; uint32_t crc32; uint8_t pad[12];
        legacy_record_t rec[LEGACY_RECORDS];
    } v1;
    memset(&v1, 0xff, sizeof(v1));
    v1.magic = 0x54434652; v1.version = 1; v1.header_size = 32; v1.generation = 2;
    v1.record_size = 64; v1.record_count = LEGACY_RECORDS;
    memset(v1.rec[0].name, 0, LEGACY_NAME_LEN);
    strcpy(v1.rec[0].name, name);
    v1.rec[0].start = start; v1.rec[0].size = size;
    v1.rec[0].reserved = rf_erase_len(size);
    v1.rec[0].crc32 = esp_rom_crc32_le(0, d, size);
    v1.rec[0].format = 1; v1.rec[0].status = LEGACY_ST_VALID;
    v1.crc32 = 0;
    v1.crc32 = esp_rom_crc32_le(0, (uint8_t *)&v1, sizeof(v1));
    memcpy(mock_flash + RF_LEGACY_CAT_B, &v1, sizeof(v1));
    uint32_t commit = 0x21544d43;
    memcpy(mock_flash + RF_LEGACY_CAT_B + RF_SECTOR_SIZE - 4, &commit, 4);
    memcpy(mock_flash + start, d, size);
}

static void test_long_titles(void)
{
    printf("long titles (name + continuation)\n");
    mock_flash_reset(0xff);
    slot_store_open();
    uint8_t *d = test_image(4096, 3);
    const char *t52 = "Leisure Suit Larry in the Land of the Lounge Lizards";
    const char *t39 = "Exactly thirty-nine characters long ok!";
    char out[SLOT_TITLE_SIZE];
    CHECK(strlen(t52) == 52 && strlen(t39) == 39);
    CHECK(upload(0, t52, d, 4096) == ESP_OK);
    CHECK(upload(1, t39, d, 4096) == ESP_OK);
    CHECK(upload(2, "Short", d, 4096) == ESP_OK);
    CHECK(slot_store_open() == SLOT_STORE_VALID);
    slot_record_title(slot_store_record(0), out); CHECK(strcmp(out, t52) == 0);
    slot_record_title(slot_store_record(1), out); CHECK(strcmp(out, t39) == 0);
    slot_record_title(slot_store_record(2), out); CHECK(strcmp(out, "Short") == 0);
    CHECK(slot_store_find_name(t52) == 0 && slot_store_find_name(t39) == 1);
    /* Old records: name field only, continuation still 0xFF. */
    CHECK((uint8_t)slot_store_record(1)->name_ext[0] == 0xff);
    CHECK((uint8_t)slot_store_record(2)->name_ext[0] == 0xff);
    free(d);
}

static void test_legacy_migration(void)
{
    printf("legacy v1 catalog migration\n");
    mock_flash_reset(0xff);
    uint8_t *cc = test_image(368640, 11);
    write_legacy("Crystal Castles", 0x010000, cc, 368640);
    uint32_t legacy_crc = region_crc(RF_LEGACY_CAT_A, 2 * RF_SECTOR_SIZE);
    uint32_t data_crc = region_crc(rf_slot_start(0), RF_SLOT_SIZE);

    CHECK(legacy_catalog_open());
    CHECK(slot_store_open() == SLOT_STORE_BLANK);
    int n = 0;
    CHECK(slot_store_migrate_legacy(&n) == ESP_OK && n == 1);
    CHECK(slot_store_find_name("Crystal Castles") == 0);
    CHECK(slot_store_record(0)->size == 368640);
    CHECK(region_crc(RF_LEGACY_CAT_A, 2 * RF_SECTOR_SIZE) == legacy_crc);  /* untouched */
    CHECK(region_crc(rf_slot_start(0), RF_SLOT_SIZE) == data_crc);        /* not moved/erased */
    for (int i = 0; i < mock_erase_count; i++) {
        CHECK(mock_erases[i].addr == RF_SLOT_CAT_A || mock_erases[i].addr == RF_SLOT_CAT_B);
    }
    /* Not repeated on an initialised store. */
    CHECK(slot_store_migrate_legacy(&n) == ESP_ERR_INVALID_STATE);

    /* Legacy image not at a slot start: refused, nothing written. */
    mock_flash_reset(0xff);
    write_legacy("Odd", 0x06A000, cc, 368640);
    slot_store_open();
    CHECK(slot_store_migrate_legacy(&n) == ESP_ERR_NOT_FOUND && n == 0);
    CHECK(slot_store_state() == SLOT_STORE_BLANK);
    free(cc);
}

int main(void)
{
    test_layout();
    test_blank_and_invalid();
    test_upload_and_bounds();
    test_crc_and_interrupt();
    test_ab_fallback();
    test_long_titles();
    test_legacy_migration();
    printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL TESTS PASSED\n", failures);
    return failures != 0;
}
