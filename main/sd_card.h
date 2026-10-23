/*
 * The SD card in J5 (board revision 2): SPI mode on the bus shared with
 * the NOR flash (own CS), FAT with long file names, mounted at SD_MOUNT.
 *
 * The user's root directory (setting root_dir, see settings_root_valid) is
 * where RadioFloppy works on the card; it is created when it does not
 * exist. Paths are only ever built from a validated root below SD_MOUNT.
 * A card that is inserted later is mounted on the next status check; a
 * removed card is noticed and unmounted. All card access goes through one
 * lock (one SPI device must not be used from two tasks at once).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SD_MOUNT    "/sd"

typedef struct {
    bool present;           /* mounted */
    uint32_t size_mb;       /* card capacity */
    bool root_exists;       /* the root directory is there (and a folder) */
} sd_status_t;

/* Mount the card (if any) and make sure the configured root exists. */
void sd_card_init(void);

/* Current state; tries to mount a card that was inserted since (at most
 * every few seconds) and notices a removed one. */
void sd_card_status(sd_status_t *st);

/*
 * Create root (a valid root_dir, may be "") on the card, with every folder
 * on the way. ESP_OK when it exists afterwards; ESP_ERR_NOT_FOUND: no
 * card; ESP_ERR_INVALID_ARG: not a valid root; ESP_FAIL: could not create.
 */
esp_err_t sd_card_make_root(const char *root);
