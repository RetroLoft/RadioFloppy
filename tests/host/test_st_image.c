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
    CHECK(st_check_size(368640, &info) == ST_OK && info.heads == 1 && info.sectors == 9);
    CHECK(st_check_size(737280, &info) == ST_OK && info.heads == 2);
    CHECK(st_check_size(0, &info) == ST_INVALID);
    CHECK(st_check_size(368641, &info) == ST_INVALID);          /* not whole sectors */
    CHECK(st_check_size(ST_MAX_SIZE + 1, &info) == ST_TOO_LARGE);
    CHECK(st_check_size(2 * 1024 * 1024, &info) == ST_TOO_LARGE);
    CHECK(st_check_size(409600, &info) == ST_UNSUPPORTED);      /* 80/1/10 */
    CHECK(st_check_size(819200, &info) == ST_UNSUPPORTED);      /* 80/2/10 */
    CHECK(st_check_size(839680, &info) == ST_TOO_LARGE);        /* 82/2/10 > 800 KiB */
    CHECK(st_check_size(512, &info) == ST_INVALID);             /* one sector */
    CHECK(st_check_size(7 * 512, &info) == ST_INVALID);

    printf("boot sector checks\n");
    memset(img, 0xe5, sizeof(img));
    CHECK(st_check_image(img, 368640, &info) == ST_OK && !info.bpb_ok);   /* custom boot */
    put16(img + 11, 512); put16(img + 24, 9); put16(img + 26, 1); put16(img + 19, 720);
    CHECK(st_check_image(img, 368640, &info) == ST_OK && info.bpb_ok);
    put16(img + 26, 2);                       /* BPB says 40 cyl / 2 sides */
    CHECK(st_check_image(img, 368640, &info) == ST_UNSUPPORTED);
    put16(img + 26, 2); put16(img + 19, 1440);
    CHECK(st_check_image(img, 737280, &info) == ST_OK && info.bpb_ok);
    put16(img + 24, 10); put16(img + 19, 1600);   /* 10 sectors in a 720k-sized file */
    CHECK(st_check_image(img, 737280, &info) == ST_UNSUPPORTED);

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

    printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL TESTS PASSED\n", failures);
    return failures != 0;
}
