/*
 * The active floppy: MFM tracks in PSRAM, where they came from, and safe
 * switching to another image.
 *
 * Two track buffers (A/B, 2 MB each) live in PSRAM. A new image is always
 * encoded and verified into the inactive buffer; only then is the active
 * pointer swapped, under the drive lock and only while our drive is not
 * selected (drive_swap_media). The flux stream reads the tracks through
 * disk_track_raw() and never waits for any of this.
 *
 * At start-up the image named DISK_BOOT_IMAGE (else the first valid image
 * in sequence order) is loaded from the external flash image library; with
 * none, the drive has no disk.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mfm_track.h"

#define DISK_BOOT_IMAGE             "Crystal Castles"

/*
 * DISK_BACKGROUND_VERIFY 1: after every disk change, decode the new tracks
 * again in the background and report the result in the log. Not needed
 * for operation (the encoder is deterministic and tested); it never delays
 * a disk change and is aborted by the next one.
 */
#define DISK_BACKGROUND_VERIFY      1

#define DISK_MAX_CYLS       84  /* head positions 0..83 */
#define DISK_MAX_HEADS      2   /* the drive always has two heads */
#define DISK_TRACKS_BYTES   ((size_t)DISK_MAX_CYLS * DISK_MAX_HEADS * MFM_MAX_BYTES)

typedef enum {
    DISK_SRC_NONE,          /* no disk inserted */
    DISK_SRC_FLASH,         /* from the image library (external flash) */
    DISK_SRC_PSRAM,         /* temporary upload, lost at reset */
} disk_source_t;

typedef struct {
    disk_source_t source;
    uint16_t image_id;      /* library image for DISK_SRC_FLASH, else 0 */
    bool image_changed;     /* that image was replaced/deleted since */
    char name[53];          /* title, up to 52 characters (IMG_TITLE_SIZE) */
    uint32_t size;
    uint32_t crc32;
    int cylinders;          /* geometry of the image */
    int heads;
    int sectors;            /* per track */
} disk_info_t;

/* Active tracks, [cyl][head][MFM_MAX_BYTES]; read by the flux ISR. */
extern uint8_t *volatile disk_tracks;

/* Bitcells per track of the active disk (100000, longer for 11 sectors). */
extern volatile uint32_t disk_track_cells;

/* Raw MFM bitcells of one track of the active disk. ISR safe. */
static inline const uint8_t *disk_track_raw(int cyl, int head)
{
    return disk_tracks + (size_t)(cyl * DISK_MAX_HEADS + head) * MFM_MAX_BYTES;
}

/* External flash, image library, buffers and the start-up image. */
esp_err_t disk_image_init(void);

/* Copy of the current disk info. */
void disk_get_current(disk_info_t *info);

/*
 * Encode raw (a validated .ST image with the geometry in info) into the
 * inactive track buffer. Not visible to the Atari yet.
 */
esp_err_t disk_prepare(const uint8_t *raw, const disk_info_t *info);

/* After a switch: decode the active tracks again in the background and
 * compare with raw (copied; result only in the log). */
void disk_verify_active(const uint8_t *raw, const disk_info_t *info);

/*
 * Make the prepared buffer the active disk. Waits up to timeout_ms for our
 * drive to be deselected; ESP_ERR_TIMEOUT (= drive busy) leaves the
 * current disk untouched.
 */
esp_err_t disk_activate_prepared(const disk_info_t *info, uint32_t timeout_ms);

/* Called (task context) after every change of the active disk. */
void disk_set_change_callback(void (*cb)(void));

/* A library image was replaced or deleted: flag the current disk if it came from there. */
void disk_note_image_changed(uint16_t image_id);
