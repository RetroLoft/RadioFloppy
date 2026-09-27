/*
 * Host test for the blank Atari disk (main/st_format.c): layout checked
 * byte for byte, accepted by the .ST validation, and every track encoded
 * to MFM and decoded again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfm_track.h"
#include "st_format.h"
#include "st_image.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static unsigned le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static unsigned boot_sum(const uint8_t *b)
{
    unsigned sum = 0;
    for (int i = 0; i < 512; i += 2) {
        sum += (b[i] << 8) | b[i + 1];
    }
    return sum & 0xffff;
}

int main(void)
{
    uint8_t *d = malloc(ST_BLANK_SIZE);
    st_format_blank(d, 0x123456);

    printf("layout\n");
    CHECK(ST_BLANK_SIZE == 737280);
    CHECK(le16(d + 11) == 512 && d[13] == 2 && le16(d + 14) == 1 && d[16] == 2);
    CHECK(le16(d + 17) == 112 && le16(d + 19) == 1440 && d[21] == 0xf9);
    CHECK(le16(d + 22) == 5 && le16(d + 24) == 9 && le16(d + 26) == 2);
    CHECK(d[8] == 0x56 && d[9] == 0x34 && d[10] == 0x12);          /* serial */
    CHECK(boot_sum(d) != 0x1234);                                   /* not executable */
    for (int f = 0; f < 2; f++) {                                   /* FAT 1 and 2 */
        const uint8_t *fat = d + (1 + f * 5) * 512;
        CHECK(fat[0] == 0xf9 && fat[1] == 0xff && fat[2] == 0xff);
        int rest = 0;
        for (int i = 3; i < 5 * 512; i++) rest |= fat[i];
        CHECK(rest == 0);                                           /* all clusters free */
    }
    int root = 0;
    for (int i = 11 * 512; i < 18 * 512; i++) root |= d[i];
    CHECK(root == 0);                                               /* empty root directory */
    CHECK(d[18 * 512] == 0xe5 && d[ST_BLANK_SIZE - 1] == 0xe5);     /* unused data */

    /* Serials change the disk, never make it executable. */
    for (uint32_t s = 0; s < 0x20000; s++) {
        uint8_t boot[512];
        st_format_blank(d, s);
        memcpy(boot, d, 512);
        if (boot_sum(boot) == 0x1234) {
            CHECK(!"executable boot sector");
            break;
        }
    }

    printf(".ST validation\n");
    st_format_blank(d, 0xabcdef);
    st_info_t info;
    CHECK(st_check_image(d, ST_BLANK_SIZE, &info) == ST_OK);
    CHECK(info.bpb_ok && info.cylinders == 80 && info.heads == 2 && info.sectors == 9);

    printf("MFM round trip\n");
    mfm_layout_t layout = mfm_layout(9);
    uint8_t *track = malloc(MFM_MAX_BYTES);
    int good = 0;
    for (int cyl = 0; cyl < 80; cyl++) {
        for (int head = 0; head < 2; head++) {
            const uint8_t *sec = d + (size_t)(cyl * 2 + head) * 9 * 512;
            mfm_build_track(track, &layout, sec, cyl, head);
            good += mfm_verify_track(track, &layout, sec, cyl, head) == 9;
        }
    }
    CHECK(good == 160);
    free(track);
    free(d);
    printf(failures ? "FAILED (%d)\n" : "blank disk OK\n", failures);
    return failures != 0;
}
