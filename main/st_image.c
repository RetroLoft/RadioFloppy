/*
 * .ST image validation. See st_image.h.
 */
#include <stdio.h>
#include <string.h>

#include "st_image.h"

#define SECTOR      512
#define SUP_CYLS    80
#define SUP_SECS    9

static int le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

/* Could size be a .ST dump with some common Atari geometry? */
static int plausible_geometry(uint32_t sectors, int *cyl, int *spt, int *heads)
{
    for (int h = 1; h <= 2; h++) {
        for (int s = 8; s <= 11; s++) {
            for (int c = 40; c <= 86; c++) {
                if ((uint32_t)(c * s * h) == sectors) {
                    *cyl = c;
                    *spt = s;
                    *heads = h;
                    return 1;
                }
            }
        }
    }
    return 0;
}

st_result_t st_check_size(uint32_t size, st_info_t *info)
{
    memset(info, 0, sizeof(*info));

    if (size == 0) {
        snprintf(info->detail, sizeof(info->detail), "empty file");
        return ST_INVALID;
    }
    if (size > ST_MAX_SIZE) {
        snprintf(info->detail, sizeof(info->detail), "%lu bytes, maximum is %u",
                 (unsigned long)size, ST_MAX_SIZE);
        return ST_TOO_LARGE;
    }
    if (size % SECTOR) {
        snprintf(info->detail, sizeof(info->detail),
                 "%lu bytes is not a whole number of 512-byte sectors", (unsigned long)size);
        return ST_INVALID;
    }
    for (int h = 1; h <= 2; h++) {
        if (size == (uint32_t)SUP_CYLS * SUP_SECS * h * SECTOR) {
            info->cylinders = SUP_CYLS;
            info->sectors = SUP_SECS;
            info->heads = h;
            return ST_OK;
        }
    }
    int c, s, h;
    if (plausible_geometry(size / SECTOR, &c, &s, &h)) {
        snprintf(info->detail, sizeof(info->detail),
                 "%lu bytes looks like %d cyl / %d sides / %d sectors; supported is "
                 "80 cyl / 1-2 sides / 9 sectors", (unsigned long)size, c, h, s);
        return ST_UNSUPPORTED;
    }
    snprintf(info->detail, sizeof(info->detail),
             "%lu bytes matches no floppy geometry", (unsigned long)size);
    return ST_INVALID;
}

st_result_t st_check_image(const uint8_t *data, uint32_t size, st_info_t *info)
{
    st_result_t r = st_check_size(size, info);
    if (r != ST_OK) {
        return r;
    }

    /* Boot sector BPB. Many game disks have a custom boot sector, so a
     * missing or odd BPB is only a remark; a *consistent* BPB that
     * describes another geometry means the size is misleading. */
    int bps = le16(data + 11), spt = le16(data + 24), heads = le16(data + 26);
    int total = le16(data + 19);
    if (bps == SECTOR && spt >= 8 && spt <= 11 && heads >= 1 && heads <= 2 &&
        total > 0 && total % (spt * heads) == 0) {
        if (spt != info->sectors || heads != info->heads ||
            (uint32_t)total * SECTOR != size) {
            snprintf(info->detail, sizeof(info->detail),
                     "boot sector describes %d sides / %d sectors / %d total sectors, "
                     "file size says %d sides / 9 sectors", heads, spt, total, info->heads);
            return ST_UNSUPPORTED;
        }
        info->bpb_ok = 1;
    } else {
        snprintf(info->detail, sizeof(info->detail),
                 "no standard BPB in the boot sector (custom boot sector?)");
    }
    return ST_OK;
}
