/*
 * Building HFE files in memory for the host tests (test_hfe.c,
 * test_hfe_import.c). f must hold HFE_BUILD_MAX bytes.
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HFE_BUILD_MAX (4u << 20)

/* ---- Building HFE files -------------------------------------------------------- */

typedef struct {
    uint8_t *side[2];       /* file bytes of each side (already with opcodes) */
    uint32_t len;           /* bytes per side */
} build_track_t;

/* Header + LUT at block 1 + tracks from block 2 on. */
static inline uint32_t build(uint8_t *f, const char *sig, uint8_t rev, int cyls, int sides,
                      build_track_t *t)
{
    memset(f, 0xff, HFE_BUILD_MAX);
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
static inline uint8_t *filler(uint32_t len, uint8_t seed)
{
    uint8_t *p = malloc(len);
    const uint8_t pat[] = { 0x44, 0x22, 0x49, 0x92, 0x24, 0x89 };   /* low nibble never 0xF */
    for (uint32_t i = 0; i < len; i++) p[i] = pat[(i + seed) % sizeof(pat)];
    return p;
}

