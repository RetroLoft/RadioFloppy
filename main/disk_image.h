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
 * At start-up the image that was active at power-off (settings), else the
 * one named DISK_BOOT_IMAGE, else the first valid image in sequence order
 * is loaded from the external flash image library; with
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
    bool keep_sectors;      /* READ_WRITE .ST image: keep its sectors for writing */
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

/* Stop that check and free its copy: compressing an image for the store
 * needs the PSRAM more. */
void disk_verify_cancel(void);

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

/* ---- Writing (see disk_write.h) ---------------------------------------------- */

/* +1 for every disk change; a write belongs to the disk of its generation. */
extern volatile uint32_t disk_media_gen;

/* The active disk is a library image whose sectors are kept (can be written). */
bool disk_writable_data(void);

/* The active disk was set to READ_WRITE after it was loaded: load and keep
 * its sectors now (ESP_ERR_NO_MEM when PSRAM is short). */
esp_err_t disk_keep_sectors(void);

/*
 * A sector written by the computer: patch the kept sectors, encode the
 * track again into the active track buffer and mark the storage block as
 * changed. ESP_ERR_INVALID_STATE: the disk changed since generation gen.
 */
esp_err_t disk_write_sector(uint32_t gen, int cyl, int head, int sector, const uint8_t *data);

/* Unsaved written sectors? */
bool disk_dirty(void);

typedef struct {
    uint8_t *data;          /* copy of the whole image (heap, caller frees) */
    uint32_t size;
    uint32_t mask;          /* changed storage blocks */
    uint16_t image_id;
    uint32_t gen;
} disk_snapshot_t;

/* Copy of the image with its changed blocks; the changes count as saved
 * from here on (ESP_ERR_NOT_FOUND: nothing to save). */
esp_err_t disk_snapshot(disk_snapshot_t *snap);

/* Saving the snapshot failed: its blocks are unsaved again. */
void disk_snapshot_failed(const disk_snapshot_t *snap);
