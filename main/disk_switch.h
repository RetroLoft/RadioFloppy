/*
 * Changing the active disk: the one place that does it, for the HTTP API
 * and the push buttons alike (serialised by one mutex).
 *
 * Disks come from the SD card (.ST and HFE files below the root directory)
 * or are a temporary upload in PSRAM. The buttons step through the image
 * files of the folder of the disk chosen last (through the API or the web
 * page), in the order the web page shows them; at the first or the last
 * file that button does nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "disk_image.h"

typedef struct {
    const char *code;       /* API error code, e.g. "DRIVE_BUSY" */
    char msg[176];
} switch_error_t;

void disk_switch_init(void);

/* Off (unsupported machine profile): every switch fails with
 * MACHINE_NOT_SUPPORTED and the drive is never touched. */
void disk_switch_set_enabled(bool on);

/* Make raw (a validated .ST image in PSRAM, geometry in info) the active disk. */
esp_err_t disk_switch_raw(const uint8_t *raw, const disk_info_t *info, switch_error_t *e);

/* The same for a temporary upload: on success raw is kept (owned here) as
 * the PSRAM disk, the previous one is freed. */
esp_err_t disk_switch_psram_upload(uint8_t *raw, const disk_info_t *info, switch_error_t *e);

/* Info of the kept PSRAM disk; false if there is none. */
bool disk_switch_psram_info(disk_info_t *info);

/*
 * Insert the .ST or .HFE file path (below the SD card's root directory,
 * sd_path.h rules), write-protected. Its folder becomes the one the
 * buttons step through. Errors: SD_NOT_PRESENT, FILE_NOT_FOUND,
 * INVALID_IMAGE, UNSUPPORTED_GEOMETRY, UNSUPPORTED_FORMAT, SD_ERROR,
 * INSUFFICIENT_MEMORY, DRIVE_BUSY, MACHINE_NOT_SUPPORTED.
 */
esp_err_t disk_switch_sd(const char *path, switch_error_t *e);

/* Buttons: dir +1 = the next image file in that folder, -1 = the previous
 * one. At the end of the list: ESP_ERR_INVALID_STATE (code "END"). */
esp_err_t disk_switch_step(int dir, switch_error_t *e);
