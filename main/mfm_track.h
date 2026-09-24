/*
 * IBM/ISO MFM track generation for Atari ST DD disks (512-byte sectors,
 * 9, 10 or 11 per track, 250 kbit/s, 300 rpm). Layout follows FlashFloppy
 * src/image/img.c for .ST images: no IAM, GAP4a 80, GAP2 22, GAP3 per
 * sector count (9: 84, 10: 30, 11: 3 with interleave 2), pre-index gap up
 * to the track length.
 *
 * A track normally has 100000 bitcells (200 ms at 2 us). 11 sectors do not
 * fit: that track is longer and is played with slightly shorter bitcells
 * so one revolution still takes 200 ms (as FlashFloppy does).
 *
 * Plain C, no ESP-IDF dependencies (also built in the host tests).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MFM_SECTOR_SIZE     512
#define MFM_MIN_SECTORS     9
#define MFM_MAX_SECTORS     11
#define MFM_TRACK_CELLS     100000      /* standard track: 200 ms / 2 us */
#define MFM_MAX_CELLS       104000      /* room for the longest layout */
#define MFM_MAX_BYTES       (MFM_MAX_CELLS / 8)     /* track buffer stride */

/*
 * TOS formats 720 kB disks with a sector skew (FlashFloppy: cskew 4,
 * hskew 2). Off by default: with skew, side 0 and side 1 have different
 * sector orders at the same rotational position, and the short delay of a
 * side change in our stream could then pair an ID of one side with the
 * data of the other. Without skew both sides line up, which is safe.
 */
#ifndef MFM_USE_TOS_SKEW
#define MFM_USE_TOS_SKEW    0
#endif

typedef struct {
    int sectors;            /* per track, 9..11 */
    int gap3;
    int interleave;
    uint32_t cells;         /* bitcells per track (multiple of 32) */
} mfm_layout_t;

/* Track layout for a number of 512-byte sectors (9..11). */
mfm_layout_t mfm_layout(int sectors);

/*
 * Build one track as raw MFM bitcells (MSB first, 1 = flux transition),
 * layout->cells / 8 bytes. sectors: layout->sectors * 512 bytes, sector 1
 * first.
 */
void mfm_build_track(uint8_t *raw, const mfm_layout_t *layout, const uint8_t *sectors,
                     uint8_t cyl, uint8_t head);

/*
 * Unformatted track of the given length (only 0x4E gap bytes, no sectors):
 * side 1 of a single-sided image, cylinders beyond the image.
 */
void mfm_build_blank_track(uint8_t *raw, uint32_t cells);

/*
 * Decode a raw track and check it: every sector must be found once with a
 * valid ID (cyl/head/R/N) and data CRC, and its data must equal sectors.
 * Returns the number of good sectors (layout->sectors when the track is
 * OK), or a negative value on a structural error.
 */
int mfm_verify_track(const uint8_t *raw, const mfm_layout_t *layout, const uint8_t *sectors,
                     uint8_t cyl, uint8_t head);

uint16_t mfm_crc16(const uint8_t *buf, int len, uint16_t crc);
