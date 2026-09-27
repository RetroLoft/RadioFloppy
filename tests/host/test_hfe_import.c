/*
 * Host tests for storing HFE uploads as a stream (main/hfe_import.c) with
 * the real image store on the RAM flash model and the host stand-in codec:
 * an image appears only after a complete, verified upload; everything
 * else leaves no image and no used blocks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "hfe_build.h"
#include "hfe_import.h"
#include "image_store.h"
#include "img_codec.h"
#include "mock_ext_flash.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MIB (1024u * 1024u)
#define TRACKS (84u * 2 * 13000)        /* the device's track buffer */

extern int host_codec_corrupt;

static uint8_t f[HFE_BUILD_MAX];

/* 80 cylinders, 2 sides; `varied` tracks with changing bytes (they do not
 * compress with the stand-in codec), the rest constant. */
static uint32_t make(const char *sig, int varied)
{
    build_track_t t[80];
    for (int c = 0; c < 80; c++) {
        t[c].len = 12500;
        t[c].side[0] = c < varied ? filler(12500, c) : NULL;
        t[c].side[1] = c < varied ? filler(12500, c + 1) : NULL;
    }
    uint32_t size = build(f, sig, 0, 80, 2, t);
    for (int c = 0; c < varied; c++) { free(t[c].side[0]); free(t[c].side[1]); }
    return size;
}

/* Feed in random pieces; returns the first error. */
static hfe_import_error_t feed(hfe_import_t *imp, const uint8_t *d, uint32_t n)
{
    hfe_import_error_t e = HFE_IMPORT_OK;
    for (uint32_t o = 0; o < n && e == HFE_IMPORT_OK;) {
        uint32_t k = (uint32_t)(rand() % 20000) + 1;
        if (k > n - o) k = n - o;
        e = hfe_import_write(imp, d + o, k);
        o += k;
    }
    return e;
}

static uint8_t *out_buf;
static uint32_t out_len;
static esp_err_t collect(void *ctx, const uint8_t *data, size_t len)
{
    memcpy(out_buf + out_len, data, len);
    out_len += (uint32_t)len;
    return ESP_OK;
}
static esp_err_t read_rec(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    return image_store_read(ctx, off, buf, (uint32_t)len);
}

/* The stored image decompresses to exactly the file. */
static int stored_equals(uint16_t id, const uint8_t *file, uint32_t size)
{
    image_record_t r;
    if (!image_store_get(id, &r)) return 0;
    out_buf = malloc(size + 65536);
    out_len = 0;
    size_t total = 0;
    int ok = img_inflate_stream(r.stored_size, read_rec, &r, collect, NULL, &total) == ESP_OK &&
             total == size && out_len == size && memcmp(out_buf, file, size) == 0;
    free(out_buf);
    return ok;
}

static void usage(store_usage_t *u) { image_store_usage(u); }
static int unchanged(const store_usage_t *a)
{
    store_usage_t b;
    image_store_usage(&b);
    return a->images == b.images && a->blocks_used == b.blocks_used;
}

static void test_import(void)
{
    mock_flash_setup(16 * MIB, 0xff);
    CHECK(image_store_open(16 * MIB) == STORE_VALID);
    store_usage_t u0;
    hfe_import_t *imp;
    uint32_t crc = 0;
    uint16_t id = 0;

    printf("complete upload -> one verified image\n");
    uint32_t size = make("HXCPICFE", 6);
    usage(&u0);
    CHECK(hfe_import_begin(size, TRACKS, &imp) == HFE_IMPORT_OK);
    CHECK(feed(imp, f, size) == HFE_IMPORT_OK);
    CHECK(image_store_find_name("Game") == 0);            /* nothing before the commit */
    CHECK(hfe_import_end(imp, &crc) == HFE_IMPORT_OK && crc == esp_rom_crc32_le(0, f, size));
    CHECK(hfe_import_info(imp)->version == 1 && hfe_import_info(imp)->cylinders == 80);
    CHECK(image_store_find_name("Game") == 0);
    CHECK(hfe_import_commit(imp, 0, "Game", &id) == HFE_IMPORT_OK && id);
    image_record_t r;
    CHECK(image_store_get(id, &r) && r.status == IMG_VALID && image_format(&r) == IMG_FMT_HFE &&
          !image_read_write(&r) && r.storage_format == IMG_STORE_DEFLATE && r.original_size == size &&
          r.crc32 == crc && r.stored_size < size / 4);
    CHECK(stored_equals(id, f, size));
    CHECK(image_store_open(16 * MIB) == STORE_VALID && image_store_get(id, &r));   /* after a restart */

    printf("HFEv3 -> format hfe3; replacing keeps id and position\n");
    uint32_t size3 = make("HXCHFEV3", 2);
    f[1024 + 3000] = 0x8f;                              /* an INDEX opcode on track 0 side 0 */
    CHECK(hfe_import_begin(size3, TRACKS, &imp) == HFE_IMPORT_OK);
    CHECK(feed(imp, f, size3) == HFE_IMPORT_OK && hfe_import_end(imp, &crc) == HFE_IMPORT_OK);
    uint16_t id3 = 0;
    CHECK(hfe_import_commit(imp, id, "Game v3", &id3) == HFE_IMPORT_OK && id3 == id);
    image_record_t r3;
    CHECK(image_store_get(id, &r3) && image_format(&r3) == IMG_FMT_HFE3 && r3.sequence == r.sequence);
    CHECK(stored_equals(id, f, size3));

    printf("interrupted: no image, blocks free\n");
    size = make("HXCPICFE", 6);
    usage(&u0);
    CHECK(hfe_import_begin(size, TRACKS, &imp) == HFE_IMPORT_OK);
    CHECK(feed(imp, f, size / 2) == HFE_IMPORT_OK);
    hfe_import_abort(imp);
    CHECK(unchanged(&u0));
    /* ... also when the data stops short and end is called. */
    CHECK(hfe_import_begin(size, TRACKS, &imp) == HFE_IMPORT_OK);
    CHECK(feed(imp, f, size - 100) == HFE_IMPORT_OK);
    CHECK(hfe_import_end(imp, &crc) == HFE_IMPORT_FORMAT);
    CHECK(hfe_import_commit(imp, 0, "Short", &id) == HFE_IMPORT_FORMAT);
    CHECK(unchanged(&u0) && !image_store_find_name("Short"));

    printf("refused early: HFEv2 after the header\n");
    f[8] = 1;
    CHECK(hfe_import_begin(size, TRACKS, &imp) == HFE_IMPORT_OK);
    CHECK(hfe_import_write(imp, f, 600) == HFE_IMPORT_FORMAT);
    CHECK(hfe_import_result(imp) == HFE_V2 && strstr(hfe_import_info(imp)->detail, "HFEv2"));
    CHECK(hfe_import_write(imp, f + 600, 100) == HFE_IMPORT_FORMAT);   /* sticky */
    hfe_import_abort(imp);
    f[8] = 0;
    CHECK(unchanged(&u0));

    printf("tracks do not fit the track buffer\n");
    CHECK(hfe_import_begin(size, 1000000, &imp) == HFE_IMPORT_OK);
    CHECK(feed(imp, f, size) == HFE_IMPORT_OK);
    CHECK(hfe_import_end(imp, &crc) == HFE_IMPORT_FORMAT && hfe_import_result(imp) == HFE_TOO_LARGE);
    hfe_import_abort(imp);
    CHECK(unchanged(&u0));

    printf("too large also compressed (more than one image's blocks)\n");
    uint32_t big = make("HXCPICFE", 80);                /* stand-in codec: doubles */
    CHECK(hfe_import_begin(big, TRACKS, &imp) == HFE_IMPORT_OK);
    hfe_import_error_t e = feed(imp, f, big);
    if (e == HFE_IMPORT_OK) e = hfe_import_end(imp, &crc);
    CHECK(e == HFE_IMPORT_STORED_TOO_LARGE);
    hfe_import_abort(imp);
    CHECK(unchanged(&u0));

    printf("stored data corrupt when read back: refused\n");
    size = make("HXCPICFE", 6);
    CHECK(hfe_import_begin(size, TRACKS, &imp) == HFE_IMPORT_OK);
    CHECK(feed(imp, f, size) == HFE_IMPORT_OK);
    host_codec_corrupt = 1;
    CHECK(hfe_import_end(imp, &crc) == HFE_IMPORT_VERIFY);
    host_codec_corrupt = 0;
    CHECK(hfe_import_commit(imp, 0, "Corrupt", &id) == HFE_IMPORT_VERIFY);
    CHECK(unchanged(&u0) && !image_store_find_name("Corrupt"));

    printf("one import at a time\n");
    hfe_import_t *imp2;
    CHECK(hfe_import_begin(size, TRACKS, &imp) == HFE_IMPORT_OK);
    CHECK(hfe_import_begin(size, TRACKS, &imp2) == HFE_IMPORT_BUSY && !imp2);
    hfe_import_abort(imp);

    printf("flash full\n");
    uint8_t *st = malloc(RF_MAX_IMAGE_SIZE);
    memset(st, 0x5a, RF_MAX_IMAGE_SIZE);
    for (int i = 0; ; i++) {
        store_usage_t u;
        image_store_usage(&u);
        if (u.blocks_free == 0) break;
        uint32_t n = u.blocks_free * 65536u < RF_MAX_IMAGE_SIZE ? u.blocks_free * 65536u : RF_MAX_IMAGE_SIZE;
        uint16_t x;
        if (image_store_save(0, "Fill", IMG_FMT_ST, st, n, &x) != ESP_OK) break;
    }
    free(st);
    usage(&u0);
    CHECK(hfe_import_begin(size, TRACKS, &imp) == HFE_IMPORT_OK);
    e = feed(imp, f, size);
    if (e == HFE_IMPORT_OK) e = hfe_import_end(imp, &crc);
    CHECK(e == HFE_IMPORT_NO_SPACE);
    hfe_import_abort(imp);
    CHECK(unchanged(&u0));
    CHECK(mock_program_violations == 0 && mock_out_of_range == 0);
}

int main(void)
{
    test_import();
    printf(failures ? "FAILED (%d)\n" : "HFE import OK\n", failures);
    return failures != 0;
}
