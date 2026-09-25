/*
 * .ST image validation. See st_image.h.
 */
#include <stdio.h>
#include <string.h>

#include "st_image.h"

#define SECTOR 512

static int le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static int supported(int cyl, int spt, int heads)
{
    return cyl >= ST_MIN_CYLS && cyl <= ST_MAX_CYLS &&
           spt >= ST_MIN_SECTORS && spt <= ST_MAX_SECTORS && heads >= 1 && heads <= 2;
}

/*
 * Supported geometry for this size with the given sectors/sides (0 = any),
 * preferring 9, then 10, then 11 sectors and one side. 0 if none.
 */
static int find_geometry(uint32_t size, int want_spt, int want_heads, int *cyl, int *spt,
                         int *heads)
{
    uint32_t sectors = size / SECTOR;

    for (int s = ST_MIN_SECTORS; s <= ST_MAX_SECTORS; s++) {
        for (int h = 1; h <= 2; h++) {
            if ((want_spt && s != want_spt) || (want_heads && h != want_heads) ||
                sectors % (s * h)) {
                continue;
            }
            int c = sectors / (s * h);
            if (supported(c, s, h)) {
                *cyl = c;
                *spt = s;
                *heads = h;
                return 1;
            }
        }
    }
    return 0;
}

/*
 * Could size be a .ST dump with some Atari geometry at all? Double density
 * (8-11 sectors) is tried first, so that e.g. 79/2/9 is not reported as
 * 79/1/18; then HD/ED (12-21 sectors), reported as unsupported.
 */
static int plausible_geometry(uint32_t sectors, int *cyl, int *spt, int *heads)
{
    static const int range[2][2] = { { 8, 11 }, { 12, 21 } };

    for (int pass = 0; pass < 2; pass++) {
        for (int h = 1; h <= 2; h++) {
            for (int s = range[pass][0]; s <= range[pass][1]; s++) {
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
    if (find_geometry(size, 0, 0, &info->cylinders, &info->sectors, &info->heads)) {
        return ST_OK;
    }
    int c, s, h;
    if (plausible_geometry(size / SECTOR, &c, &s, &h)) {
        snprintf(info->detail, sizeof(info->detail),
                 "%lu bytes looks like %d cyl / %d sides / %d sectors; supported is "
                 "79-84 cyl / 1-2 sides / 9-11 sectors", (unsigned long)size, c, h, s);
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

    /*
     * The size alone decides the geometry: no supported size fits two
     * geometries. The boot sector (BPB) is only reported: many game disks
     * have a custom or stale one, and bad dumps can be shorter than their
     * file system (the missing tracks then read as unformatted, as on the
     * real disk).
     */
    int bps = le16(data + 11), spt = le16(data + 24), heads = le16(data + 26);
    int total = le16(data + 19);
    if (bps != SECTOR || spt < 8 || spt > 11 || heads < 1 || heads > 2 || total <= 0) {
        snprintf(info->detail, sizeof(info->detail),
                 "no standard BPB in the boot sector (custom boot sector?)");
        return ST_OK;
    }
    if (spt == info->sectors && heads == info->heads &&
        (uint32_t)total * SECTOR <= size) {
        info->bpb_ok = 1;
        return ST_OK;
    }
    snprintf(info->detail, sizeof(info->detail),
             "boot sector: %d sides / %d sectors / %d total; played as the file size says: "
             "%d cyl / %d sides / %d sectors%s", heads, spt, total,
             info->cylinders, info->heads, info->sectors,
             (uint32_t)total * SECTOR > size ? " (image shorter than file system)" : "");
    return ST_OK;
}
