/*
 * The active floppy: MFM tracks in PSRAM, where they came from, and safe
 * switching to another image.
 *
 * Two track buffers (A/B, 2 MB each) live in PSRAM. A new image is always
 * encoded and verified into the inactive buffer; only then is the active
 * pointer swapped, under the drive lock and never while the computer writes
 * to the disk (drive_swap_media). The flux stream reads the tracks through
 * disk_track_raw() and never waits for any of this.
 *
 * At start-up the disk that was active at power-off (settings last_sd) is
 * read from the SD card, or from its copy in the flash (disk_copy.h) when
 * the card is not there; with none, the drive has no disk.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mfm_track.h"
#include "sd_path.h"

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
    DISK_SRC_PSRAM,         /* temporary upload, lost at reset */
    DISK_SRC_SD,            /* a file on the SD card */
} disk_source_t;

typedef struct {
    disk_source_t source;
    bool image_changed;     /* its file was replaced/deleted since */
    char name[53];          /* title, up to 52 characters (IMG_TITLE_SIZE) */
    uint32_t size;
    uint32_t crc32;
    int cylinders;          /* geometry of the image */
    int heads;
    int sectors;            /* per track */
    bool keep_sectors;      /* .ST from the SD card: sectors kept, so it can be written */
    bool hfe;               /* HFE image: packed tracks (hfe.h), played in native timing */
    char sd_path[SD_REL_MAX + 1];   /* DISK_SRC_SD: the file, below the root directory */
    bool sd_copy;           /* DISK_SRC_SD read from its copy in the flash (card not there) */
    bool read_only;         /* DISK_SRC_SD: the file has the FAT read-only attribute */
    uint32_t file_crc;      /* DISK_SRC_SD: CRC-32 of the file on the card as last known
                             * (differs from crc32 while written sectors are not on it) */
} disk_info_t;

/* Active tracks, [cyl][head][MFM_MAX_BYTES]; read by the flux ISR. */
extern uint8_t *volatile disk_tracks;

/* Bitcells per track of the active disk (100000, longer for 11 sectors). */
extern volatile uint32_t disk_track_cells;

/* What the active track buffer holds: MFM tracks of an .ST image
 * ([cyl][head][MFM_MAX_BYTES]) or packed HFE tracks (hfe.h). */
typedef enum { DISK_TRACKS_MFM, DISK_TRACKS_HFE } disk_tracks_kind_t;
extern volatile uint8_t disk_tracks_kind;

/* Raw MFM bitcells of one track of the active disk. ISR safe. */
static inline const uint8_t *disk_track_raw(int cyl, int head)
{
    return disk_tracks + (size_t)(cyl * DISK_MAX_HEADS + head) * MFM_MAX_BYTES;
}

/* External flash, buffers and the start-up disk. */
esp_err_t disk_image_init(void);

/* Copy of the current disk info. */
void disk_get_current(disk_info_t *info);

/*
 * Encode raw (a validated .ST image with the geometry in info) into the
 * inactive track buffer. Not visible to the Atari yet.
 */
esp_err_t disk_prepare(const uint8_t *raw, const disk_info_t *info);

/*
 * The same for the HFE file info->sd_path on the SD card: read straight
 * into the inactive buffer as packed tracks, never whole in memory. Fills
 * in size, CRC, cylinders and heads. ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND
 * (no card), ESP_FAIL (not a usable HFE).
 */
esp_err_t disk_prepare_hfe(disk_info_t *info);

/* After a switch: decode the active tracks again in the background and
 * compare with raw (copied; result only in the log). */
void disk_verify_active(const uint8_t *raw, const disk_info_t *info);

/* Stop that check and free its copy: compressing an image for the store
 * needs the PSRAM more. */
void disk_verify_cancel(void);

/* A disk change first waits this long for our drive to be deselected, so an
 * access in progress can finish; after that it also swaps while the drive
 * stays selected (see drive_swap_media). */
#define DISK_SWAP_SELECTED_AFTER_MS 300

/*
 * Make the prepared buffer the active disk. Waits up to timeout_ms for a
 * moment it may swap (drive_swap_media): deselected, or after
 * DISK_SWAP_SELECTED_AFTER_MS also selected unless the computer writes to a
 * writable disk. ESP_ERR_TIMEOUT (= drive busy) leaves the current disk
 * untouched.
 */
esp_err_t disk_activate_prepared(const disk_info_t *info, uint32_t timeout_ms);

/* Called (task context) after every change of the active disk. */
void disk_set_change_callback(void (*cb)(void));

/* A file on the SD card was replaced or deleted (folder: everything in
 * folder path): flag the current disk if it came from there. */
void disk_note_sd_changed(const char *path, bool folder);

/* The root directory setting changed: the current SD disk's path now
 * means another file (flagged as changed). */
void disk_note_root_changed(void);

/* File path got / lost the read-only attribute. */
void disk_note_sd_read_only(const char *path, bool ro);

/* Written sectors of the disk of generation gen were saved: the disk's
 * CRC-32 is crc now, and the file on the card has file_crc. */
void disk_note_saved(uint32_t gen, uint32_t crc, uint32_t file_crc);

/* ---- Writing (see disk_write.h) ---------------------------------------------- */

/* +1 for every disk change; a write belongs to the disk of its generation. */
extern volatile uint32_t disk_media_gen;

/* The active disk's sectors are kept (it can be written). */
bool disk_writable_data(void);

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
    uint32_t mask;          /* changed 64 KiB regions */
    uint32_t gen;
} disk_snapshot_t;

/* Copy of the image with its changed blocks; the changes count as saved
 * from here on (ESP_ERR_NOT_FOUND: nothing to save). */
esp_err_t disk_snapshot(disk_snapshot_t *snap);

/* Saving the snapshot failed: its blocks are unsaved again. */
void disk_snapshot_failed(const disk_snapshot_t *snap);
