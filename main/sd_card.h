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

/* ---- Folders and files below the root directory (paths: sd_path.h) ------- */

typedef enum {
    SD_OK,
    SD_NO_CARD,             /* no card mounted */
    SD_INVALID,             /* not a valid relative path / name */
    SD_NOT_FOUND,           /* folder or file does not exist */
    SD_EXISTS,              /* already there */
    SD_NOT_EMPTY,           /* folder has contents (and no recursive delete) */
    SD_IO,                  /* card error (removed, full, ...) */
} sd_result_t;

typedef struct {
    const char *name;
    bool is_dir;
    uint32_t size;
} sd_entry_t;

/*
 * The entries of folder rel ("" = the root directory), hidden names
 * (starting with '.') left out, at most max of them (*more: there were
 * more). cb runs with the card lock held: it must not use the card.
 */
sd_result_t sd_card_list(const char *rel, int max, void (*cb)(void *ctx, const sd_entry_t *e),
                         void *ctx, bool *more);

/* What rel is: SD_OK with *is_dir (and *size for a file), or SD_NOT_FOUND. */
sd_result_t sd_card_stat(const char *rel, bool *is_dir, uint32_t *size);

/* Create folder rel with every folder on the way (SD_EXISTS: it was already there). */
sd_result_t sd_card_mkdir(const char *rel);

/* Remove folder rel (not the root directory): empty, or with everything
 * in it when recursive. */
sd_result_t sd_card_rmdir(const char *rel, bool recursive);

/* Remove file rel. */
sd_result_t sd_card_remove(const char *rel);

/*
 * Writing a file in pieces: into a hidden temporary file in the target
 * folder; only sd_card_writer_commit (after reading it back and checking
 * the CRC-32) gives it its name, replacing an existing file only when
 * overwrite was set. Anything else leaves no file behind.
 */
typedef struct sd_writer sd_writer_t;
sd_result_t sd_card_writer_open(const char *folder, const char *name, bool overwrite,
                                sd_writer_t **w);
sd_result_t sd_card_writer_write(sd_writer_t *w, const void *data, uint32_t len);
sd_result_t sd_card_writer_commit(sd_writer_t *w, uint32_t crc);     /* frees w */
void sd_card_writer_abort(sd_writer_t *w);                           /* NULL is fine */

const char *sd_result_name(sd_result_t r);
