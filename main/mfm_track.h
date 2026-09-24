/*
 * IBM/ISO MFM track generation for Atari ST DD disks (9 x 512 bytes,
 * 250 kbit/s, 300 rpm). Layout follows FlashFloppy src/image/img.c for
 * .ST images: no IAM, GAP4a 80, GAP2 22, GAP3 84, pre-index gap fills the
 * track to exactly 100000 bitcells (200 ms at 2 us per cell).
 *
 * Plain C, no ESP-IDF dependencies (also built in the host test).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MFM_SECTORS         9
#define MFM_SECTOR_SIZE     512
#define MFM_TRACK_CELLS     100000      /* 200 ms / 2 us */
#define MFM_TRACK_BYTES     (MFM_TRACK_CELLS / 8)   /* raw bitcell bytes */

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

/*
 * Build one track as raw MFM bitcells (MSB first, 1 = flux transition).
 * sectors: MFM_SECTORS * MFM_SECTOR_SIZE bytes, sector 1 first.
 */
void mfm_build_track(uint8_t raw[MFM_TRACK_BYTES], const uint8_t *sectors,
                     uint8_t cyl, uint8_t head);

/*
 * Unformatted track (only 0x4E gap bytes, no sectors): side 1 of a
 * single-sided image, like a single-sided disk in a double-sided drive.
 */
void mfm_build_blank_track(uint8_t raw[MFM_TRACK_BYTES]);

/*
 * Decode a raw track and check it: every sector must be found once with a
 * valid ID (cyl/head/R/N) and data CRC, and its data must equal sectors.
 * Returns the number of good sectors (MFM_SECTORS when the track is OK).
 */
int mfm_verify_track(const uint8_t raw[MFM_TRACK_BYTES], const uint8_t *sectors,
                     uint8_t cyl, uint8_t head);

uint16_t mfm_crc16(const uint8_t *buf, int len, uint16_t crc);
