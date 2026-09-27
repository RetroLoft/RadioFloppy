/*
 * Host tests for the HFE parser (main/hfe.c): files built in memory (v1
 * and v3 with every opcode) decoded bit for bit, broken and unsupported
 * files refused, and real HFE files when present.
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

/* ---- Building HFE files -------------------------------------------------------- */

typedef struct {
    uint8_t *side[2];       /* file bytes of each side (already with opcodes) */
    uint32_t len;           /* bytes per side */
} build_track_t;

/* Header + LUT at block 1 + tracks from block 2 on. */
static uint32_t build(uint8_t *f, const char *sig, uint8_t rev, int cyls, int sides,
                      build_track_t *t)
{
    memset(f, 0xff, BUF);
    memcpy(f, sig, 8);
    f[8] = rev; f[9] = cyls; f[10] = sides; f[11] = 0;
    f[12] = 250 & 0xff; f[13] = 250 >> 8; f[14] = 0; f[15] = 0;
    f[16] = 2; f[17] = 1; f[18] = 1; f[19] = 0;     /* LUT at block 1 */
    f[20] = 0; f[21] = 0xff;
    uint32_t blk = 2;
    for (int c = 0; c < cyls; c++) {
        uint32_t len = t[c].len, blocks = (len + 255) / 256;
        f[512 + 4 * c] = blk & 0xff; f[513 + 4 * c] = blk >> 8;
        f[514 + 4 * c] = (len * 2) & 0xff; f[515 + 4 * c] = (len * 2) >> 8;
        for (uint32_t i = 0; i < len; i++) {
            for (int s = 0; s < 2; s++) {
                f[blk * 512 + (i / 256) * 512 + s * 256 + i % 256] = t[c].side[s] ? t[c].side[s][i] : 0x44;
            }
        }
        blk += blocks;
    }
    return blk * 512;
}

/* MFM-like filler: 0x44 (LSB first: cells 0,0,1,0,0,0,1,0) has no opcode nibble. */
static uint8_t *filler(uint32_t len, uint8_t seed)
{
    uint8_t *p = malloc(len);
    const uint8_t pat[] = { 0x44, 0x22, 0x49, 0x92, 0x24, 0x89 };   /* low nibble never 0xF */
    for (uint32_t i = 0; i < len; i++) p[i] = pat[(i + seed) % sizeof(pat)];
    return p;
}

static int cell(const uint8_t *out, uint32_t i) { return (out[i >> 3] >> (7 - (i & 7))) & 1; }

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

    static uint8_t out[20000];
    hfe_track_t trk;
    for (int c = 0; c < 80; c += 39) {
        for (int s = 0; s < 2; s++) {
            CHECK(hfe_decode_track(f, size, &info, c, s, out, sizeof(out), &trk) == HFE_OK);
            CHECK(trk.cells == 100000 && trk.nseg == 1 && trk.seg[0].ns_x16 == 32000 && trk.nweak == 0);
            int ok = 1;
            for (uint32_t i = 0; i < trk.cells && ok; i++) ok = cell(out, i) == file_bit(t[c].side[s], i);
            CHECK(ok);          /* both sides de-interleaved, LSB-first bit order */
        }
    }
    /* Too small a buffer: refused as not playable. */
    CHECK(hfe_check(f, size, 1000000, &info) == HFE_TOO_LARGE);
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
    static uint8_t out[20000];
    hfe_track_t trk;
    CHECK(hfe_decode_track(f, size, &info, 0, 0, out, sizeof(out), &trk) == HFE_OK);
    CHECK(trk.cells == n);
    CHECK(trk.index_cell == 0);
    /* Rotated: new cell 0 = old index_at. */
    int ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) {
        uint32_t old = (i + index_at) % n;
        int is_weak = old >= weak_at && old < weak_at + 16;
        if (!is_weak) ok = cell(out, i) == cell(exp, old);
    }
    CHECK(ok);
    /* Timing after rotation: 2 us from cell 0 (the index lies after the
     * change back to 2 us), then 2.22 us from the old BITRATE 80, 2 us again. */
    CHECK(trk.nseg == 3);
    CHECK(trk.seg[0].start == 0 && trk.seg[0].ns_x16 == 32000);
    CHECK(trk.seg[1].start == br80_at + n - index_at && trk.seg[1].ns_x16 == 80u * 32000 / 72);
    CHECK(trk.seg[2].start == br72_at + n - index_at && trk.seg[2].ns_x16 == 32000);
    CHECK(trk.nweak == 1 && trk.weak[0].start == weak_at + n - index_at && trk.weak[0].count == 16);

    /* Other tracks: no opcodes, index at the start. */
    CHECK(hfe_decode_track(f, size, &info, 1, 1, out, sizeof(out), &trk) == HFE_OK);
    CHECK(trk.cells == len * 8 && trk.nseg == 1 && trk.nweak == 0);

    /* The same bytes in a v1 file are plain data (0x0F etc. are cells). */
    memcpy(f, "HXCPICFE", 8);
    CHECK(hfe_check(f, size, BUF, &info) == HFE_OK && info.opcodes == 0);
    CHECK(hfe_decode_track(f, size, &info, 0, 0, out, sizeof(out), &trk) == HFE_OK && trk.cells == len * 8);
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
    static uint8_t out[20000];
    hfe_track_t trk;
    CHECK(hfe_check(f, size, BUF, &info) == HFE_OK && info.sides == 1);
    CHECK(hfe_decode_track(f, size, &info, 1, 1, out, sizeof(out), &trk) == HFE_OK && trk.cells == 0);
    CHECK(hfe_decode_track(f, size, &info, 5, 0, out, sizeof(out), &trk) == HFE_BAD_TRACKS);
    for (int c = 0; c < 3; c++) free(t[c].side[0]);
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
    static uint8_t out[40000];
    hfe_track_t trk;
    CHECK(hfe_decode_track(f, size, &info, 0, 0, out, sizeof(out), &trk) == HFE_OK);
    int syncs = 0;
    uint16_t w = 0;
    for (uint32_t i = 0; i < trk.cells; i++) {
        w = (w << 1) | cell(out, i);
        syncs += w == 0x4489;
    }
    CHECK(syncs >= 27);         /* 9+ sectors: ID and data field, 3 syncs each */
}

int main(int argc, char **argv)
{
    printf("HFEv1\n");          test_v1();
    printf("HFEv3 opcodes\n");  test_v3();
    printf("refused files\n");  test_refused();
    printf("real files\n");
    for (int i = 1; i < argc; i++) test_real(argv[i]);
    printf(failures ? "FAILED (%d)\n" : "HFE OK\n", failures);
    return failures != 0;
}
