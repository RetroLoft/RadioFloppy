/*
 * The (embedded, read-only) floppy image and its pre-encoded MFM tracks.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "mfm_track.h"

/*
 * Embedded images (images/ directory). Change only this line to select
 * the image the emulator presents:
 *   DISK_IMAGE_CRYSTAL_CASTLES  CRYSTAL_CASTLES.ST      (360 kB, 80/1/9)
 *                               local only, not in the repository
 *   DISK_IMAGE_RETROLOFT        RETROLOFT_TEST_720K.ST  (720 kB, 80/2/9)
 * If the selected image is not embedded, RETROLOFT_TEST_720K.ST is used.
 */
#define DISK_IMAGE_SELECT   DISK_IMAGE_CRYSTAL_CASTLES

#define DISK_IMAGE_RETROLOFT        0
#define DISK_IMAGE_CRYSTAL_CASTLES  1

#define DISK_CYLINDERS      80
#define DISK_MAX_HEADS      2   /* the drive always has two heads */

/* Name and number of sides of the selected image (valid after init). */
extern const char *disk_image_name;
extern int disk_image_heads;

/* All tracks as raw MFM bitcells, [cyl][head][MFM_TRACK_BYTES], in PSRAM. */
extern uint8_t *disk_tracks;

/* Check the embedded image and encode all tracks (verified in background). */
esp_err_t disk_image_init(void);

/* Raw MFM bitcells of one track. Usable from ISR context. */
static inline const uint8_t *disk_track_raw(int cyl, int head)
{
    return disk_tracks + (size_t)(cyl * DISK_MAX_HEADS + head) * MFM_TRACK_BYTES;
}
