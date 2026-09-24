/*
 * The floppy image (from the external SPI flash) and its pre-encoded MFM
 * tracks.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "mfm_track.h"

/*
 * The firmware contains no floppy images: the image named
 * DISK_EXTERNAL_IMAGE is read from the external SPI flash (image store, see
 * image_store.h) into PSRAM before the MFM tracks are built. If it is
 * missing or corrupt the drive stays disabled.
 */
#define DISK_EXTERNAL_IMAGE         "Crystal Castles"

#define DISK_CYLINDERS      80
#define DISK_MAX_HEADS      2   /* the drive always has two heads */

/* Name, source and number of sides of the image in use (after init). */
extern const char *disk_image_name;
extern const char *disk_image_source;
extern int disk_image_heads;

/* All tracks as raw MFM bitcells, [cyl][head][MFM_TRACK_BYTES], in PSRAM. */
extern uint8_t *disk_tracks;

/* Load the image from the external flash and encode all tracks. */
esp_err_t disk_image_init(void);

/* Raw MFM bitcells of one track. Usable from ISR context. */
static inline const uint8_t *disk_track_raw(int cyl, int head)
{
    return disk_tracks + (size_t)(cyl * DISK_MAX_HEADS + head) * MFM_TRACK_BYTES;
}
