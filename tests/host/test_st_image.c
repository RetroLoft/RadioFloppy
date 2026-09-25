/*
 * Host tests for the .ST validation (main/st_image.c).
 *   tests/host/run.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st_image.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint8_t *load(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    static uint8_t buf[ST_MAX_SIZE + 1];
    *size = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return buf;
}

static void put16(uint8_t *p, int v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

int main(void)
{
    st_info_t info;
    static uint8_t img[ST_MAX_SIZE + 4096];

    printf("size checks\n");
    CHECK(st_check_size(368640, &info) == ST_OK && info.cylinders == 80 && info.heads == 1 && info.sectors == 9);
    CHECK(st_check_size(737280, &info) == ST_OK && info.cylinders == 80 && info.heads == 2 && info.sectors == 9);
    CHECK(st_check_size(409600, &info) == ST_OK && info.cylinders == 80 && info.heads == 1 && info.sectors == 10);
    CHECK(st_check_size(419840, &info) == ST_OK && info.cylinders == 82 && info.heads == 1 && info.sectors == 10);
    CHECK(st_check_size(450560, &info) == ST_OK && info.cylinders == 80 && info.heads == 1 && info.sectors == 11);
    CHECK(st_check_size(473088, &info) == ST_OK && info.cylinders == 84 && info.sectors == 11);   /* 84/1/11 */
    CHECK(st_check_size(746496, &info) == ST_OK && info.cylinders == 81 && info.heads == 2);      /* 81/2/9 */
    CHECK(st_check_size(819200, &info) == ST_OK && info.cylinders == 80 && info.heads == 2 && info.sectors == 10);
    CHECK(st_check_size(0, &info) == ST_INVALID);
    CHECK(st_check_size(368641, &info) == ST_INVALID);          /* not whole sectors */
    CHECK(st_check_size(ST_MAX_SIZE + 1, &info) == ST_TOO_LARGE);
    CHECK(st_check_size(839680, &info) == ST_OK && info.cylinders == 82);   /* 82/2/10 */
    CHECK(st_check_size(901120, &info) == ST_OK && info.sectors == 11);      /* 80/2/11 */
    CHECK(st_check_size(946176, &info) == ST_OK && info.cylinders == 84 && info.sectors == 11);
    CHECK(st_check_size(1474560, &info) == ST_UNSUPPORTED);     /* HD 80/2/18 */
    CHECK(st_check_size(1572864, &info) != ST_OK && st_check_size(1572864, &info) != ST_TOO_LARGE);
    CHECK(st_check_size(1572865, &info) == ST_TOO_LARGE);
    CHECK(st_check_size(645120, &info) == ST_UNSUPPORTED);      /* 70/2/9 */
    CHECK(st_check_size(728064, &info) == ST_OK && info.cylinders == 79 && info.heads == 2 &&
          info.sectors == 9);                                   /* 79/2/9, e.g. Baby Jo */
    CHECK(st_check_size(404480, &info) == ST_OK && info.cylinders == 79 && info.heads == 1);
    CHECK(st_check_size(808960, &info) == ST_OK && info.cylinders == 79 && info.sectors == 10);
    CHECK(st_check_size(718848, &info) == ST_UNSUPPORTED &&     /* 78/2/9: DD, not 78/1/18 */
          strstr(info.detail, "78 cyl / 2 sides / 9 sectors") != NULL);
    CHECK(st_check_size(327680, &info) == ST_UNSUPPORTED);      /* 80/1/8 */
    CHECK(st_check_size(512, &info) == ST_INVALID);
    CHECK(st_check_size(7 * 512, &info) == ST_INVALID);
    /* every supported size maps to exactly one geometry */
    for (int c = 80; c <= 84; c++)
        for (int sp = 9; sp <= 11; sp++)
            for (int h = 1; h <= 2; h++) {
                uint32_t sz = (uint32_t)c * sp * h * 512;
                if (sz > ST_MAX_SIZE) continue;
                CHECK(st_check_size(sz, &info) == ST_OK && info.cylinders == c &&
                      info.sectors == sp && info.heads == h);
            }

    printf("boot sector checks\n");
    memset(img, 0xe5, sizeof(img));
    CHECK(st_check_image(img, 368640, &info) == ST_OK && !info.bpb_ok);   /* custom boot */
    put16(img + 11, 512); put16(img + 24, 9); put16(img + 26, 1); put16(img + 19, 720);
    CHECK(st_check_image(img, 368640, &info) == ST_OK && info.bpb_ok);
    put16(img + 26, 2);                       /* BPB says 40 cyl / 2 sides */
    CHECK(st_check_image(img, 368640, &info) == ST_OK && !info.bpb_ok && info.heads == 1 &&
          strstr(info.detail, "played as the file size says") != NULL);
    put16(img + 26, 2); put16(img + 19, 1440);
    CHECK(st_check_image(img, 737280, &info) == ST_OK && info.bpb_ok);
    put16(img + 24, 10); put16(img + 19, 1600);   /* 10 sectors in a 720k-sized file */
    CHECK(st_check_image(img, 737280, &info) == ST_OK && !info.bpb_ok && info.sectors == 9);
    put16(img + 26, 1); put16(img + 19, 820);     /* Road Runner [b]: 82 tracks in the BPB, */
    CHECK(st_check_image(img, 409600, &info) == ST_OK && info.cylinders == 80 &&  /* 80 dumped */
          info.sectors == 10 && !info.bpb_ok && strstr(info.detail, "shorter") != NULL);
    put16(img + 24, 9); put16(img + 26, 1); put16(img + 19, 720);   /* 80 of 82 tracks used */
    CHECK(st_check_image(img, 82 * 9 * 512, &info) == ST_OK && info.cylinders == 82 && info.bpb_ok);

    printf("real images\n");
    uint32_t size;
    uint8_t *d = load("../../images/CRYSTAL_CASTLES.ST", &size);
    if (d) {
        CHECK(st_check_image(d, size, &info) == ST_OK && info.heads == 1 && info.bpb_ok);
    } else {
        printf("  (CRYSTAL_CASTLES.ST not present, skipped)\n");
    }
    d = load("../../images/RETROLOFT_TEST_720K.ST", &size);
    CHECK(d && st_check_image(d, size, &info) == ST_OK && info.heads == 2 && info.bpb_ok);
    d = load("../../images/NEBULUS.ST", &size);
    if (d) {
        CHECK(st_check_image(d, size, &info) == ST_OK && info.cylinders == 82 &&
              info.heads == 1 && info.sectors == 10 && info.bpb_ok);
    } else {
        printf("  (NEBULUS.ST not present, skipped)\n");
    }

    printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL TESTS PASSED\n", failures);
    return failures != 0;
}
