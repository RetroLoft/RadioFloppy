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
 * At start-up the image named DISK_BOOT_IMAGE (else the first valid slot)
 * is loaded from the external flash; with none, the drive has no disk.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mfm_track.h"

#define DISK_BOOT_IMAGE             "Crystal Castles"

/*
 * DISK_MIGRATE_LEGACY 1: on a blank slot store, take over legacy v1 images
 * that already sit at a slot start (catalog write only, no image bytes are
 * moved or erased). Needed before the API may write to the slots, because
 * slot 1 still holds the legacy Crystal Castles bytes.
 */
#define DISK_MIGRATE_LEGACY         1

/*
 * DISK_BACKGROUND_VERIFY 1: after every disk change, decode the new tracks
 * again in the background and report the result in the log. Not needed
 * for operation (the encoder is deterministic and tested); it never delays
 * a disk change and is aborted by the next one.
 */
#define DISK_BACKGROUND_VERIFY      1

#define DISK_CYLINDERS      80
#define DISK_MAX_HEADS      2   /* the drive always has two heads */
#define DISK_TRACKS_BYTES   ((size_t)DISK_CYLINDERS * DISK_MAX_HEADS * MFM_TRACK_BYTES)

typedef enum {
    DISK_SRC_NONE,          /* no disk inserted */
    DISK_SRC_FLASH,         /* from an external flash slot */
    DISK_SRC_PSRAM,         /* temporary upload, lost at reset */
} disk_source_t;

typedef struct {
    disk_source_t source;
    int slot;               /* 0..19 for DISK_SRC_FLASH, else -1 */
    bool slot_changed;      /* the slot was overwritten/deleted since */
    char name[40];
    uint32_t size;
    uint32_t crc32;
    int heads;
} disk_info_t;

/* Active tracks, [cyl][head][MFM_TRACK_BYTES]; read by the flux ISR. */
extern uint8_t *volatile disk_tracks;

/* Raw MFM bitcells of one track of the active disk. ISR safe. */
static inline const uint8_t *disk_track_raw(int cyl, int head)
{
    return disk_tracks + (size_t)(cyl * DISK_MAX_HEADS + head) * MFM_TRACK_BYTES;
}

/* External flash, slot store, buffers and the start-up image. */
esp_err_t disk_image_init(void);

/* Copy of the current disk info. */
void disk_get_current(disk_info_t *info);

/*
 * Encode raw (a validated .ST image, heads 1 or 2) into the inactive track
 * buffer. Not visible to the Atari yet.
 */
esp_err_t disk_prepare(const uint8_t *raw, int heads);

/* After a switch: decode the active tracks again in the background and
 * compare with raw (copied; result only in the log). */
void disk_verify_active(const uint8_t *raw, uint32_t size, int heads);

/*
 * Make the prepared buffer the active disk. Waits up to timeout_ms for our
 * drive to be deselected; ESP_ERR_TIMEOUT (= drive busy) leaves the
 * current disk untouched.
 */
esp_err_t disk_activate_prepared(const disk_info_t *info, uint32_t timeout_ms);

/* A flash slot was overwritten or deleted: flag the current disk if it came from there. */
void disk_note_slot_changed(int slot);
