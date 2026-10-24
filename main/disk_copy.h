/*
 * One copy of the last disk from the SD card in the external flash
 * (RF_COPY_HEADER, RF_COPY_DATA; flash_layout.h), so that disk also comes
 * back at start-up without the card. The file itself, uncompressed.
 *
 * Writing: the header is erased first (no valid copy), then the data is
 * written and read back (CRC-32), and only then the header with its
 * commit word. A power cut in between leaves no copy, never a wrong one.
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
    char path[SD_REL_MAX + 1];   /* the file on the card, below the root directory */
} disk_copy_info_t;

/* The copy's header, if there is a valid copy. */
bool disk_copy_get(disk_copy_info_t *info);

/* Read len bytes at off of the copy's data. */
esp_err_t disk_copy_read(uint32_t off, void *buf, uint32_t len);

/* Writing in pieces (one writer at a time). ESP_ERR_INVALID_SIZE: larger
 * than RF_COPY_MAX. The old copy is gone from begin on. */
esp_err_t disk_copy_begin(uint32_t size);
esp_err_t disk_copy_write(const void *data, uint32_t len);
/* All data written: check it and make it the valid copy. */
esp_err_t disk_copy_commit(const disk_copy_info_t *info);

/* No copy any more (e.g. the disk on the card was deleted). */
esp_err_t disk_copy_clear(void);
