/*
 * Host tests for the HFE reader (main/hfe.c): files built in memory (v1
 * and v3 with every opcode) streamed into a packed track buffer and
 * checked bit for bit, in one piece and in random pieces; the fit check
 * equal to the real packing; broken, unsupported and out-of-order files
 * refused; real HFE files when present.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hfe.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define BUF (4u << 20)

static uint8_t rev8(uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r |= ((b >> i) & 1) << (7 - i);
    return r;
}

#include "hfe_build.h"

static int cell(const uint8_t *out, uint32_t i) { return (out[i >> 3] >> (7 - (i & 7))) & 1; }

/* ---- Streaming into a packed buffer ------------------------------------------ */

static uint8_t packbuf[4u << 20];
#define AT_ONCE 0
#define RANDOM  1

/* Stream f into packbuf (capacity cap), at once or in random pieces. */
static hfe_result_t stream(const uint8_t *f, uint32_t size, int how, uint32_t cap, hfe_info_t *info)
{
    static uint8_t region[HFE_REGION_MAX];
    static hfe_stream_t s;
    hfe_pack_t p;
    hfe_pack_init(&p, packbuf, cap);
    hfe_stream_init(&s, size, region, &p);
    for (uint32_t o = 0; o < size;) {
        uint32_t n = how == RANDOM ? (uint32_t)(rand() % 9000) + 1 : size - o;
        if (n > size - o) n = size - o;
        hfe_stream_feed(&s, f + o, n);
        o += n;
    }
    hfe_result_t r = hfe_stream_end(&s);
    *info = s.info;
    return r;
}

static void packed(int cyl, int side, hfe_packed_track_t *t) { hfe_packed_track(packbuf, cyl, side, t); }

/* Streaming at once and in pieces gives the same buffer; the check's
 * count equals the packed size, and exactly that size fits. */
static void same_packing(const uint8_t *f, uint32_t size)
{
    static uint8_t first[4u << 20];
    hfe_info_t a, b, c;
    CHECK(stream(f, size, AT_ONCE, sizeof(packbuf), &a) == HFE_OK);
    memcpy(first, packbuf, a.bytes_needed);
    CHECK(stream(f, size, RANDOM, sizeof(packbuf), &b) == HFE_OK);
    CHECK(a.bytes_needed == b.bytes_needed && memcmp(first, packbuf, a.bytes_needed) == 0);
    CHECK(hfe_check(f, size, sizeof(packbuf), &c) == HFE_OK && c.bytes_needed == a.bytes_needed);
    CHECK(hfe_check(f, size, a.bytes_needed, &c) == HFE_OK);
    CHECK(hfe_check(f, size, a.bytes_needed - 4, &c) == HFE_TOO_LARGE);
    CHECK(stream(f, size, AT_ONCE, a.bytes_needed, &c) == HFE_OK);
    CHECK(stream(f, size, RANDOM, a.bytes_needed - 4, &c) == HFE_TOO_LARGE);
}

/* Expected cells of a plain byte sequence (LSB first per byte). */
static int file_bit(const uint8_t *b, uint32_t i) { return (b[i / 8] >> (i % 8)) & 1; }

static void test_v1(void)
{
    static uint8_t f[BUF];
    build_track_t t[80];
    for (int c = 0; c < 80; c++) {
        t[c].len = 12500;
        t[c].side[0] = filler(12500, c);
        t[c].side[1] = filler(12500, c + 3);
    }
    uint32_t size = build(f, "HXCPICFE", 0, 80, 2, t);
    hfe_info_t info;
    CHECK(hfe_check(f, size, BUF, &info) == HFE_OK);
    CHECK(info.version == 1 && info.cylinders == 80 && info.sides == 2 && info.bitrate == 250);
    CHECK(info.max_cells == 100000 && info.opcodes == 0);

    CHECK(stream(f, size, RANDOM, sizeof(packbuf), &info) == HFE_OK);
    hfe_packed_track_t pt;
    for (int c = 0; c < 80; c += 13) {
        for (int s = 0; s < 2; s++) {
            packed(c, s, &pt);
            CHECK(pt.cells && pt.count == 100000 && pt.nseg == 1 && pt.seg[0].start == 0 &&
                  pt.seg[0].ns_x16 == 32000 && pt.nweak == 0);
            int ok = pt.cells != NULL;
            for (uint32_t i = 0; i < pt.count && ok; i++) ok = cell(pt.cells, i) == file_bit(t[c].side[s], i);
            CHECK(ok);          /* both sides de-interleaved, LSB-first bit order */
        }
    }
    packed(80, 0, &pt);
    CHECK(pt.cells == NULL);    /* beyond the last cylinder: no track */
    same_packing(f, size);
    /* Too small a buffer: refused as not playable. */
    CHECK(hfe_check(f, size, 1000000, &info) == HFE_TOO_LARGE && strstr(info.detail, "KiB"));
    for (int c = 0; c < 80; c++) { free(t[c].side[0]); free(t[c].side[1]); }
}

static void test_v3(void)
{
    static uint8_t f[BUF];
    build_track_t t[2];
    /* Track 0 side 0 with opcodes; plain bytes elsewhere. */
    uint32_t len = 12600;
    uint8_t *s0 = filler(len, 0);
    s0[100] = 0x0f;                          /* NOP */
    s0[200] = 0x4f; s0[201] = rev8(80);      /* BITRATE: cell = 2 us * 80/72 */
    s0[300] = 0x2f;                          /* RAND: 8 weak cells */
    s0[301] = 0x2f;                          /* adjacent RAND: same area */
    s0[400] = 0xcf; s0[401] = rev8(3); s0[402] = 0x92;   /* SKIP 3 bits of 0x92 */
    s0[500] = 0x4f; s0[501] = rev8(72);      /* BITRATE back to 2 us */
    s0[6000] = 0x8f;                         /* INDEX */
    t[0].len = len; t[0].side[0] = s0; t[0].side[1] = filler(len, 5);
    t[1].len = len; t[1].side[0] = filler(len, 1); t[1].side[1] = filler(len, 2);
    uint32_t size = build(f, "HXCHFEV3", 0, 2, 2, t);

    hfe_info_t info;
    CHECK(hfe_check(f, size, BUF, &info) == HFE_OK);
    CHECK(info.version == 3 && info.opcodes == 7 && info.weak_areas == 1 && info.bitrate_changes == 2);

    /* Expected cells, before rotation: opcodes removed, SKIP drops 3 bits. */
    static uint8_t exp[20000];
    uint32_t n = 0, weak_at = 0, br80_at = 0, br72_at = 0, index_at = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = s0[i];
        unsigned first = 0;
        if (i == 100) continue;
        if (i == 200) { br80_at = n; i++; continue; }
        if (i == 400) { first = 3; i += 2; b = s0[i]; }
        if (i == 500) { br72_at = n; i++; continue; }
        if (i == 6000) { index_at = n; continue; }
        if (i == 300) weak_at = n;
        for (unsigned k = first; k < 8; k++) {
            int bit = (b >> k) & 1;
            if (bit) exp[n >> 3] |= 0x80 >> (n & 7); else exp[n >> 3] &= ~(0x80 >> (n & 7));
            n++;
        }
    }
    hfe_packed_track_t pt;
    CHECK(stream(f, size, RANDOM, sizeof(packbuf), &info) == HFE_OK);
    packed(0, 0, &pt);
    CHECK(pt.cells && pt.count == n);
    /* Rotated: new cell 0 = old index_at. */
    int ok = pt.cells != NULL;
    for (uint32_t i = 0; i < n && ok; i++) {
        uint32_t old = (i + index_at) % n;
        int is_weak = old >= weak_at && old < weak_at + 16;
        if (!is_weak) ok = cell(pt.cells, i) == cell(exp, old);
    }
    CHECK(ok);
    /* Timing after rotation: 2 us from cell 0 (the index lies after the
     * change back to 2 us), then 2.22 us from the old BITRATE 80, 2 us again. */
    CHECK(pt.nseg == 3);
    CHECK(pt.seg[0].start == 0 && pt.seg[0].ns_x16 == 32000);
    CHECK(pt.seg[1].start == br80_at + n - index_at && pt.seg[1].ns_x16 == 80u * 32000 / 72);
    CHECK(pt.seg[2].start == br72_at + n - index_at && pt.seg[2].ns_x16 == 32000);
    CHECK(pt.nweak == 1 && pt.weak[0].start == weak_at + n - index_at && pt.weak[0].count == 16);

    /* Other tracks: no opcodes, index at the start. */
    packed(1, 1, &pt);
    CHECK(pt.count == len * 8 && pt.nseg == 1 && pt.nweak == 0);
    same_packing(f, size);

    /* The same bytes in a v1 file are plain data (0x0F etc. are cells). */
    memcpy(f, "HXCPICFE", 8);
    CHECK(hfe_check(f, size, BUF, &info) == HFE_OK && info.opcodes == 0);
    CHECK(stream(f, size, AT_ONCE, sizeof(packbuf), &info) == HFE_OK);
    packed(0, 0, &pt);
    CHECK(pt.count == len * 8);
    memcpy(f, "HXCHFEV3", 8);

    /* Bad opcodes. */
    s0[700] = 0xff;                          /* reserved */
    size = build(f, "HXCHFEV3", 0, 2, 2, t);
    CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_OPCODE);
    s0[700] = 0x44;
    s0[7000] = 0x8f;                         /* a second INDEX */
    size = build(f, "HXCHFEV3", 0, 2, 2, t);
    CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_OPCODE);
    s0[7000] = 0x44;
    s0[len - 1] = 0x4f;                      /* BITRATE without its argument */
    size = build(f, "HXCHFEV3", 0, 2, 2, t);
    CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_OPCODE);
    s0[len - 1] = 0x44;
    s0[800] = 0x4f; s0[801] = rev8(10);      /* absurd bitrate */
    size = build(f, "HXCHFEV3", 0, 2, 2, t);
    CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_OPCODE);
    for (int c = 0; c < 2; c++) { free(t[c].side[0]); free(t[c].side[1]); }
}

/* A track as HxC writes it from a flux dump: a BITRATE opcode every few
 * bytes (drive jitter), the INDEX repeated just before the end, more weak
 * areas than fit. Smoothed into zones that keep the exact total time. */
static void test_flux_v3(void)
{
    static uint8_t f[BUF];
    build_track_t t[2];
    uint32_t len = 18000;
    uint8_t *s0 = filler(len, 0);
    uint64_t exact = 0;                     /* 1/16 ns, as played without smoothing */
    uint32_t ns = 32000, cells = 0, changes = 0, index_at = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (i == 30) { s0[i] = 0x8f; index_at = cells; continue; }            /* INDEX */
        if (i == len - 3) { s0[i] = 0x8f; continue; }                         /* INDEX again */
        if (i % 7 == 0 && i + 1 < len - 3) {
            uint8_t x = 70 + (i / 7) % 6;                                     /* 70..75 */
            s0[i] = 0x4f; s0[i + 1] = rev8(x);
            ns = (uint32_t)x * 32000 / 72;
            changes++;
            i++;
            continue;
        }
        if (i % 400 == 200) s0[i] = 0x2f;                                     /* RAND */
        exact += 8ull * ns;
        cells += 8;
    }
    t[0].len = len; t[0].side[0] = s0; t[0].side[1] = filler(len, 4);
    t[1].len = len; t[1].side[0] = filler(len, 1); t[1].side[1] = filler(len, 2);
    uint32_t size = build(f, "HXCHFEV3", 0, 2, 2, t);

    hfe_info_t info;
    CHECK(hfe_check(f, size, BUF, &info) == HFE_OK);
    CHECK(info.smoothed_tracks == 1 && strstr(info.detail, "smoothed on 1 track"));
    CHECK(changes > 1000 && info.opcodes > changes);
    CHECK(stream(f, size, RANDOM, sizeof(packbuf), &info) == HFE_OK);
    hfe_packed_track_t pt;
    packed(0, 0, &pt);
    CHECK(pt.cells && pt.count == cells);
    CHECK(pt.nseg >= 2 && pt.nseg <= HFE_MAX_SEGMENTS);
    CHECK(pt.nweak >= HFE_MAX_WEAK - 2 && pt.nweak <= HFE_MAX_WEAK);   /* 45 areas, joined */
    uint64_t played = 0;
    int sorted = 1;
    for (int i = 0; i < pt.nseg; i++) {
        uint32_t end = i + 1 < pt.nseg ? pt.seg[i + 1].start : pt.count;
        sorted &= end > pt.seg[i].start;
        played += (uint64_t)(end - pt.seg[i].start) * pt.seg[i].ns_x16;
        /* Every zone plays near 2 us: the jitter is averaged away. */
        CHECK(pt.seg[i].ns_x16 >= 70u * 32000 / 72 && pt.seg[i].ns_x16 <= 75u * 32000 / 72);
    }
    CHECK(pt.seg[0].start == 0 && sorted);
    /* The revolution keeps its duration: off by less than 1/16 ns per cell
     * of one zone (the last division remainder), here under 0.1 us. */
    CHECK(played <= exact && exact - played <= len * 8 / (HFE_MAX_SEGMENTS - 1) + 1);
    /* Rotated to the first INDEX; the one at the end was ignored. */
    CHECK(cell(pt.cells, 0) == file_bit(&s0[31], 0));
    (void)index_at;
    same_packing(f, size);

    /* A repeated INDEX in the middle of the revolution is still refused. */
    s0[len / 2] = 0x8f;
    size = build(f, "HXCHFEV3", 0, 2, 2, t);
    CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_OPCODE);
    for (int c = 0; c < 2; c++) { free(t[c].side[0]); free(t[c].side[1]); }
}

static void test_refused(void)
{
    static uint8_t f[BUF];
    build_track_t t[3];
    for (int c = 0; c < 3; c++) { t[c].len = 12500; t[c].side[0] = filler(12500, c); t[c].side[1] = NULL; }
    uint32_t size = build(f, "HXCPICFE", 0, 3, 2, t);
    hfe_info_t info;
    CHECK(hfe_check(f, size, BUF, &info) == HFE_OK);

    CHECK(hfe_check(f, 100, BUF, &info) == HFE_NOT_HFE);
    memcpy(f, "HXCPICFX", 8); CHECK(hfe_check(f, size, BUF, &info) == HFE_NOT_HFE);
    memcpy(f, "HXCPICFE", 8);
    f[8] = 1; CHECK(hfe_check(f, size, BUF, &info) == HFE_V2 && strstr(info.detail, "HFEv2"));
    f[8] = 2; CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_HEADER);
    f[8] = 0;
    f[11] = 2; CHECK(hfe_check(f, size, BUF, &info) == HFE_UNSUPPORTED);     /* FM */
    f[11] = 0;
    f[12] = 0xf4; f[13] = 0x01; CHECK(hfe_check(f, size, BUF, &info) == HFE_UNSUPPORTED);  /* 500 kbit/s */
    f[12] = 250; f[13] = 0;
    f[16] = 4; CHECK(hfe_check(f, size, BUF, &info) == HFE_UNSUPPORTED);     /* Amiga */
    f[16] = 2;
    f[21] = 0; CHECK(hfe_check(f, size, BUF, &info) == HFE_UNSUPPORTED);     /* double step */
    f[21] = 0xff;
    f[9] = 90; CHECK(hfe_check(f, size, BUF, &info) == HFE_UNSUPPORTED);     /* 90 cylinders */
    f[9] = 3;
    f[10] = 0; CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_HEADER);
    f[10] = 2;
    f[18] = 200; CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_TRACKS);    /* LUT outside */
    f[18] = 1;
    uint8_t keep = f[512 + 4]; f[512 + 4] = 250;                            /* track 1 outside */
    CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_TRACKS);
    f[512 + 4] = keep;
    CHECK(hfe_check(f, size - 512, BUF, &info) == HFE_BAD_TRACKS);          /* truncated file */
    f[514 + 8] = 0; f[515 + 8] = 0;                                         /* track 2 length 0 */
    CHECK(hfe_check(f, size, BUF, &info) == HFE_BAD_TRACKS);
    /* Single-sided: side 1 has no track. */
    size = build(f, "HXCPICFE", 0, 3, 1, t);
    hfe_packed_track_t pt;
    CHECK(hfe_check(f, size, BUF, &info) == HFE_OK && info.sides == 1);
    CHECK(stream(f, size, RANDOM, sizeof(packbuf), &info) == HFE_OK);
    packed(1, 0, &pt);
    CHECK(pt.cells && pt.count == 100000);
    packed(1, 1, &pt);
    CHECK(pt.cells == NULL && pt.count == 0);
    for (int c = 0; c < 3; c++) free(t[c].side[0]);
}

/* Track order: gaps and trailing data are fine; tracks before the table
 * or out of order are refused with a clear message. */
static void test_layout(void)
{
    static uint8_t f[BUF], g[BUF];
    build_track_t t[3];
    for (int c = 0; c < 3; c++) { t[c].len = 12500; t[c].side[0] = filler(12500, c); t[c].side[1] = filler(12500, c + 7); }
    uint32_t size = build(f, "HXCPICFE", 0, 3, 2, t);
    hfe_info_t info;
    static uint8_t first[1u << 20];
    CHECK(stream(f, size, AT_ONCE, sizeof(packbuf), &info) == HFE_OK);
    uint32_t used = info.bytes_needed;
    memcpy(first, packbuf, used);

    /* Last track moved 3 blocks further (a gap), plus data after it. */
    uint32_t off2 = (f[512 + 8] | f[513 + 8] << 8) * 512, region = (12500 + 255) / 256 * 512;
    memcpy(g, f, size);
    memset(g + off2, 0xee, region);
    memcpy(g + off2 + 1536, f + off2, region);
    uint32_t blk = off2 / 512 + 3;
    g[512 + 8] = blk & 0xff; g[513 + 8] = blk >> 8;
    CHECK(stream(g, size + 1536 + 700, RANDOM, sizeof(packbuf), &info) == HFE_OK);
    CHECK(info.bytes_needed == used && memcmp(first, packbuf, used) == 0);

    /* Tracks 1 and 2 swapped in the table: out of order. */
    memcpy(g, f, size);
    memcpy(g + 516, f + 520, 4); memcpy(g + 520, f + 516, 4);
    CHECK(hfe_check(g, size, BUF, &info) == HFE_BAD_LAYOUT && strstr(info.detail, "ascending"));
    /* A track inside the table's block. */
    memcpy(g, f, size);
    g[512] = 1; g[513] = 0;
    CHECK(hfe_check(g, size, BUF, &info) == HFE_BAD_LAYOUT && strstr(info.detail, "track table"));
    /* The table after the tracks (block 40, past the data would be outside). */
    memcpy(g, f, size);
    memcpy(g + size, f + 512, 512);
    g[18] = (uint8_t)(size / 512); g[19] = 0;
    CHECK(hfe_check(g, size + 512, BUF, &info) == HFE_BAD_LAYOUT);
    /* Data missing at the end, and more data than announced. */
    CHECK(stream(f, size - 1, RANDOM, sizeof(packbuf), &info) == HFE_BAD_TRACKS);
    {
        static uint8_t region[HFE_REGION_MAX];
        static hfe_stream_t s;
        hfe_pack_t p;
        hfe_pack_init(&p, NULL, BUF);
        hfe_stream_init(&s, size, region, &p);
        CHECK(hfe_stream_feed(&s, f, size / 2) == HFE_OK);
        CHECK(hfe_stream_end(&s) == HFE_BAD_TRACKS && strstr(s.info.detail, "incomplete"));
        hfe_stream_init(&s, 1000, region, &p);
        CHECK(hfe_stream_feed(&s, f, 2000) == HFE_BAD_TRACKS);
        CHECK(hfe_stream_feed(&s, f, 10) == HFE_BAD_TRACKS);      /* sticky */
    }
    for (int c = 0; c < 3; c++) { free(t[c].side[0]); free(t[c].side[1]); }
}

/* Real files: accepted, and track 0 holds MFM sector marks. */
static void test_real(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("  (%s not present, skipped)\n", path); return; }
    static uint8_t f[8u << 20];
    uint32_t size = fread(f, 1, sizeof(f), fp);
    fclose(fp);
    hfe_info_t info;
    hfe_result_t r = hfe_check(f, size, 2u << 20, &info);
    printf("  %s: %s (%s)\n", strrchr(path, '/') + 1, hfe_result_name(r), info.detail);
    CHECK(r == HFE_OK || r == HFE_TOO_LARGE);
    CHECK(hfe_check(f, size, 8u << 20, &info) == HFE_OK);
    same_packing(f, size);
    CHECK(stream(f, size, RANDOM, sizeof(packbuf), &info) == HFE_OK);
    hfe_packed_track_t pt;
    packed(0, 0, &pt);
    int syncs = 0;
    uint16_t w = 0;
    for (uint32_t i = 0; pt.cells && i < pt.count; i++) {
        w = (w << 1) | cell(pt.cells, i);
        syncs += w == 0x4489;
    }
    CHECK(syncs >= 27);         /* 9+ sectors: ID and data field, 3 syncs each */
}

int main(int argc, char **argv)
{
    printf("HFEv1\n");          test_v1();
    printf("HFEv3 opcodes\n");  test_v3();
    printf("HFEv3 from a flux dump\n"); test_flux_v3();
    printf("refused files\n");  test_refused();
    printf("track layout\n");   test_layout();
    printf("real files\n");
    for (int i = 1; i < argc; i++) test_real(argv[i]);
    printf(failures ? "FAILED (%d)\n" : "HFE OK\n", failures);
    return failures != 0;
}
