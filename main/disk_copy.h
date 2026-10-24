/*
 * One copy of the last disk from the SD card in the external flash
 * (RF_COPY_HEADER, RF_COPY_DATA; flash_layout.h), so that disk also comes
 * back at start-up without the card. The file itself, uncompressed.
 *
 * Writing: the header is erased first (no valid copy), then the data is
 * written and read back (CRC-32), and only then the header with its
 * commit word. A power cut in between leaves no copy, never a wrong one.
 *
 * The copy is also the journal for sectors the computer writes: a save
 * first patches the changed 64 KiB blocks of the copy and commits it as
 * *pending* (newer than the file on the card, which still has base_crc),
 * then writes the file on the card and marks the copy synced. A pending
 * copy found at start-up is written to the card first.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sd_path.h"

typedef struct {
    uint32_t size;
    uint32_t crc;           /* CRC-32 of the file */
    bool hfe;               /* an HFE file (else .ST) */
    bool pending;           /* newer than the file on the card (not synced yet) */
    uint32_t base_crc;      /* pending: CRC-32 of the file on the card it replaces */
    char path[SD_REL_MAX + 1];   /* the file on the card, below the root directory */
} disk_copy_info_t;

/* Before any other call (creates the lock). */
void disk_copy_init(void);

/* One user at a time for a sequence of calls (copy, save, start-up sync). */
void disk_copy_lock(void);
void disk_copy_unlock(void);

/* The copy's header, if there is a valid copy. */
bool disk_copy_get(disk_copy_info_t *info);

/* Read len bytes at off of the copy's data. */
esp_err_t disk_copy_read(uint32_t off, void *buf, uint32_t len);

/* Writing in pieces (one writer at a time). ESP_ERR_INVALID_SIZE: larger
 * than RF_COPY_MAX. The old copy is gone from begin on. */
esp_err_t disk_copy_begin(uint32_t size);
esp_err_t disk_copy_write(const void *data, uint32_t len);

/*
 * Changing some 64 KiB blocks of the valid copy instead (same size):
 * patch_begin makes the copy invalid; patch replaces the block at off
 * (a multiple of RF_COPY_BLOCK) with data, as far as the copy goes.
 */
esp_err_t disk_copy_patch_begin(uint32_t size);
esp_err_t disk_copy_patch(uint32_t off, const void *data);

/* All data written (or patched): read it back, check info->crc and make
 * it the valid copy (pending as info says). */
esp_err_t disk_copy_commit(const disk_copy_info_t *info);

/* A pending copy is on the card now. */
esp_err_t disk_copy_mark_synced(void);

/* No copy any more (e.g. the disk on the card was deleted). */
esp_err_t disk_copy_clear(void);
