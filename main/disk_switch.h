/*
 * Changing the active disk: the one place that does it, for the HTTP API
 * and the push buttons alike (serialised by one mutex).
 *
 * The disk list for the buttons: position 0 is the PSRAM image (if there
 * is one), then every valid library image in sequence order. Left = -1,
 * right = +1, wrapping around.
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

/* Make a stored library image the active disk. */
esp_err_t disk_switch_image(uint16_t image_id, switch_error_t *e);

/* Activate raw (a validated image) described by info (flash upload). */
esp_err_t disk_switch_raw(const uint8_t *raw, const disk_info_t *info, switch_error_t *e);

/*
 * Activate raw as the PSRAM image. On success the module takes ownership
 * of raw (heap, PSRAM) and frees the previous PSRAM image; on failure the
 * caller keeps it.
 */
esp_err_t disk_switch_psram_upload(uint8_t *raw, const disk_info_t *info, switch_error_t *e);

/* Step through the disk list (dir -1 / +1). */
esp_err_t disk_switch_step(int dir, switch_error_t *e);

/* Info of the kept PSRAM image; false if there is none. */
bool disk_switch_psram_info(disk_info_t *info);
