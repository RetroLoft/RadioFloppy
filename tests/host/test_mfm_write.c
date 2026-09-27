/*
 * Host tests for the write path pieces in main/mfm_track.c: the MFM write
 * decoder (a WD1772-like write stream with jitter and clock deviation) and
 * the rotational position -> sector lookup.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfm_track.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---- A minimal independent MFM writer --------------------------------------- */

static uint8_t cells[20000];
static int nc;
static int prev_bit;

static void put_cell(int c) { cells[nc++] = c; }

static void put_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        int bit = (b >> i) & 1;
        put_cell(!prev_bit && !bit);    /* clock */
        put_cell(bit);
        prev_bit = bit;
    }
}

static void put_sync(void)              /* A1 with the missing clock: 0x4489 */
{
    for (int i = 15; i >= 0; i--) {
        put_cell((0x4489 >> i) & 1);
    }
    prev_bit = 1;
}

/* What the WD1772 writes for Write Sector (from WGATE on). */
static void build_write(const uint8_t *data, uint8_t mark, int corrupt, int cut)
{
    uint8_t buf[4 + 512] = { 0xa1, 0xa1, 0xa1, mark };
    memcpy(buf + 4, data, 512);
    uint16_t crc = mfm_crc16(buf, sizeof(buf), 0xffff);

    nc = 0;
    prev_bit = 0;
    for (int i = 0; i < 12; i++) put_byte(0x00);
    for (int i = 0; i < 3; i++) put_sync();
    put_byte(mark);
    for (int i = 0; i < 512; i++) put_byte(data[i] ^ (i == 100 ? corrupt : 0));
    put_byte(crc >> 8);
    put_byte(crc & 0xff);
    put_byte(0x4e);
    if (cut) nc -= cut;
}

/* Cells -> intervals between transitions in 0.1 us ticks, with a writer
 * clock off by ppm and random jitter of +-jit ticks. */
static int to_intervals(uint16_t *iv, int ppm, int jit, unsigned seed)
{
    int n = 0, since = 0;
    double cell = 20.0 * (1.0 + ppm / 1e6), t = 0, last = -1;
    srand(seed);
    for (int i = 0; i < nc; i++) {
        t += cell;
        since++;
        if (cells[i]) {
            double now = t + (jit ? (rand() % (2 * jit + 1)) - jit : 0);
            if (last >= 0) {
                iv[n++] = (uint16_t)(now - last + 0.5);
            }
            last = now;
            since = 0;
        }
    }
    (void)since;
    return n;
}

static void test_decoder(void)
{
    static uint16_t iv[20000];
    uint8_t data[512], out[512], mark;
    for (int i = 0; i < 512; i++) data[i] = (uint8_t)(i * 7 + 3);

    struct { int ppm, jit; } cases[] = { { 0, 0 }, { 0, 3 }, { 15000, 3 }, { -15000, 3 }, { 30000, 2 } };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        build_write(data, 0xfb, 0, 0);
        int n = to_intervals(iv, cases[c].ppm, cases[c].jit, 1 + c);
        memset(out, 0, sizeof(out));
        mfm_wr_result_t r = mfm_decode_write(iv, n, 100, &mark, out);
        CHECK(r == MFM_WR_OK && mark == 0xfb && memcmp(out, data, 512) == 0);
        if (r != MFM_WR_OK) printf("    case ppm %d jit %d: result %d\n", cases[c].ppm, cases[c].jit, r);
    }

    /* All-zero and all-FF sectors (longest and shortest intervals). */
    uint8_t z[512] = { 0 }, f[512];
    memset(f, 0xff, 512);
    build_write(z, 0xfb, 0, 0);
    int n = to_intervals(iv, 5000, 3, 9);
    CHECK(mfm_decode_write(iv, n, 100, &mark, out) == MFM_WR_OK && memcmp(out, z, 512) == 0);
    build_write(f, 0xf8, 0, 0);
    n = to_intervals(iv, -5000, 3, 10);
    CHECK(mfm_decode_write(iv, n, 100, &mark, out) == MFM_WR_OK && mark == 0xf8 &&
          memcmp(out, f, 512) == 0);

    /* Errors: bad CRC, cut short, no sync, an ID field (formatting). */
    build_write(data, 0xfb, 0x10, 0);
    n = to_intervals(iv, 0, 2, 11);
    CHECK(mfm_decode_write(iv, n, 100, &mark, out) == MFM_WR_BAD_CRC);
    build_write(data, 0xfb, 0, 16 * 100);
    n = to_intervals(iv, 0, 2, 12);
    CHECK(mfm_decode_write(iv, n, 100, &mark, out) == MFM_WR_SHORT);
    for (int i = 0; i < 500; i++) iv[i] = 40 + (i % 3) * 20;   /* plain MFM, no sync */
    CHECK(mfm_decode_write(iv, 500, 100, &mark, out) == MFM_WR_NO_SYNC);
    nc = 0; prev_bit = 0;
    for (int i = 0; i < 12; i++) put_byte(0x00);
    for (int i = 0; i < 3; i++) put_sync();
    put_byte(0xfe);
    for (int i = 0; i < 6; i++) put_byte(0x01);
    n = to_intervals(iv, 0, 0, 13);
    CHECK(mfm_decode_write(iv, n, 100, &mark, out) == MFM_WR_ID_FIELD);

    /* Noise pulses before the write (glitches) are skipped. */
    build_write(data, 0xfb, 0, 0);
    n = to_intervals(iv + 3, 0, 2, 14);
    iv[0] = 3; iv[1] = 150; iv[2] = 5;
    CHECK(mfm_decode_write(iv, n + 3, 100, &mark, out) == MFM_WR_OK && memcmp(out, data, 512) == 0);
}

/* Every sector's ID end, found by decoding a built track, matches
 * mfm_id_end_cell / mfm_sector_at. */
static void test_position(void)
{
    for (int spt = 9; spt <= 11; spt++) {
        mfm_layout_t l = mfm_layout(spt);
        uint8_t *secs = calloc(spt, 512);
        uint8_t *raw = malloc(MFM_MAX_BYTES);
        mfm_build_track(raw, &l, secs, 5, 1);
        int found = 0;
        for (uint32_t i = 0; i + 64 < l.cells; i++) {
            uint16_t w = 0;
            for (int k = 0; k < 16; k++) w = (w << 1) | ((raw[(i + k) >> 3] >> (7 - ((i + k) & 7))) & 1);
            if (w != 0x4489) continue;
            /* three syncs + mark: read the mark byte */
            uint32_t p = i + 48;
            uint8_t mark = 0, r = 0;
            for (int k = 0; k < 8; k++) mark = (mark << 1) | ((raw[(p + 2 * k + 1) >> 3] >> (7 - ((p + 2 * k + 1) & 7))) & 1);
            if (mark == 0xfe) {
                uint32_t q = p + 16 * 3;        /* R byte */
                for (int k = 0; k < 8; k++) r = (r << 1) | ((raw[(q + 2 * k + 1) >> 3] >> (7 - ((q + 2 * k + 1) & 7))) & 1);
                uint32_t id_end = p + 16 * 7;   /* FE C H R N CRC CRC */
                CHECK(id_end == mfm_id_end_cell(&l, found));
                int sector = 0;
                CHECK(mfm_sector_at(&l, 5, 1, id_end + 22 * 16, &sector) == 22 * 16 && sector == r);
                found++;
            }
            i += 47;
        }
        CHECK(found == spt);
        int sector;
        CHECK(mfm_sector_at(&l, 5, 1, 100, &sector) == -1);     /* in GAP 4a */
        free(secs);
        free(raw);
    }
}

int main(void)
{
    printf("write decoder\n");  test_decoder();
    printf("position -> sector\n"); test_position();
    printf(failures ? "FAILED (%d)\n" : "MFM write OK\n", failures);
    return failures != 0;
}
