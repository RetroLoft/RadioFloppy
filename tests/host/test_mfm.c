/*
 * Host tests for the MFM track generator (main/mfm_track.c) and the flux
 * timing used by main/flux_stream.c, for 9, 10 and 11 sectors per track.
 *   tests/host/run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfm_track.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define REV_TICKS 2000000u      /* FLUX_REV_TICKS */

/* Encode + decode every track; check MFM intervals and the flux timing. */
static void check_image(const char *name, const uint8_t *img, int cyls, int heads, int spt)
{
    static uint8_t raw[MFM_MAX_BYTES];
    mfm_layout_t l = mfm_layout(spt);
    int bad = 0, bad_mfm = 0, bad_time = 0;

    for (int c = 0; c < cyls; c++) {
        for (int h = 0; h < heads; h++) {
            const uint8_t *secs = img + (size_t)(c * heads + h) * spt * 512;
            mfm_build_track(raw, &l, secs, c, h);
            if (mfm_verify_track(raw, &l, secs, c, h) != spt) {
                bad++;
            }
            /* Same arithmetic as flux_encode(): one revolution must add up
             * to exactly REV_TICKS, and intervals stay 2..4 cells. */
            uint32_t frac = 0, total = 0, dist = 0, first = 0, seen = 0;
            for (uint32_t p = 0; p < 2 * l.cells; p++) {
                uint32_t i = p % l.cells;
                dist++;
                if (raw[i >> 3] & (0x80 >> (i & 7))) {
                    if (seen && (dist < 2 || dist > 4)) {
                        bad_mfm++;
                    }
                    if (seen && p >= first + 1 && p <= first + l.cells) {
                        uint32_t num = frac + dist * REV_TICKS;
                        total += num / l.cells;
                        frac = num % l.cells;
                    }
                    if (!seen) {
                        first = p;
                        seen = 1;
                    }
                    dist = 0;
                }
            }
            if (total != REV_TICKS || frac != 0) {
                bad_time++;
            }
        }
    }
    printf("  %-18s %2d/%d/%2d: %6lu cells, %.4f us/cell, GAP3 %2d, interleave %d\n", name, cyls,
           heads, spt, (unsigned long)l.cells, 200000.0 / l.cells, l.gap3, l.interleave);
    CHECK(bad == 0);
    CHECK(bad_mfm == 0);
    CHECK(bad_time == 0);
}

static uint8_t *load(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    uint8_t *d = malloc(want);
    size_t n = fread(d, 1, want, f);
    fclose(f);
    if (n != want) {
        free(d);
        return NULL;
    }
    return d;
}

int main(void)
{
    printf("layouts\n");
    CHECK(mfm_layout(9).cells == MFM_TRACK_CELLS && mfm_layout(9).gap3 == 84);
    CHECK(mfm_layout(10).cells == MFM_TRACK_CELLS && mfm_layout(10).gap3 == 30);
    CHECK(mfm_layout(11).cells == 102848 && mfm_layout(11).gap3 == 3 &&
          mfm_layout(11).interleave == 2);
    CHECK(mfm_layout(11).cells <= MFM_MAX_CELLS);

    printf("tracks\n");
    uint8_t *d = load("../../images/RETROLOFT_TEST_720K.ST", 737280);
    CHECK(d != NULL);
    if (d) {
        check_image("RETROLOFT 720K", d, 80, 2, 9);
        free(d);
    }
    if ((d = load("../../images/CRYSTAL_CASTLES.ST", 368640))) {
        check_image("Crystal Castles", d, 80, 1, 9);
        free(d);
    }
    if ((d = load("../../images/NEBULUS.ST", 419840))) {
        check_image("Nebulus", d, 82, 1, 10);
        free(d);
    }
    size_t n = 84 * 2 * 11 * 512;
    d = malloc(n);
    srand(7);
    for (size_t i = 0; i < n; i++) {
        d[i] = rand();
    }
    check_image("random", d, 84, 2, 9);
    check_image("random", d, 84, 2, 10);
    check_image("random", d, 84, 2, 11);
    free(d);

    printf("\n%s\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED");
    return failures != 0;
}
