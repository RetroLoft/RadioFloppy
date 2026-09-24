/*
 * The (embedded, read-only) floppy image and its pre-encoded MFM tracks.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "mfm_track.h"

/*
 * Image source. With DISK_SOURCE_EXTERNAL the image named
 * DISK_EXTERNAL_IMAGE is read from the external SPI flash (image store,
 * see image_store.h) into PSRAM before the MFM tracks are built.
 *
 * DISK_PROVISION_EMBEDDED: if that image is not in the store yet, write
 *   the embedded Crystal Castles image into it once (erase/program/verify).
 * DISK_EMBEDDED_FALLBACK: if loading from the external flash fails, use
 *   the embedded image instead (clearly reported). 0 = stay disabled.
 */
#define DISK_SOURCE_EXTERNAL        1
#define DISK_EXTERNAL_IMAGE         "Crystal Castles"
#define DISK_PROVISION_EMBEDDED     1
#define DISK_EMBEDDED_FALLBACK      1

/*
 * Embedded images (images/ directory), used when DISK_SOURCE_EXTERNAL is 0,
 * for provisioning and as fallback. Change only this line to select
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

/* Name, source and number of sides of the image in use (after init). */
extern const char *disk_image_name;
extern const char *disk_image_source;
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
