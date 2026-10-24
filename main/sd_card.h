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

/* A card is mounted, as last seen (no card access: cheap). */
bool sd_card_present(void);

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
    SD_READ_ONLY,           /* the file has the read-only attribute (not overwritten) */
    SD_IO,                  /* card error (removed, full, ...) */
} sd_result_t;

typedef struct {
    const char *name;
    bool is_dir;
    uint32_t size;
    bool read_only;         /* file with the FAT read-only attribute */
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

/* File rel has the FAT read-only attribute (write-protected for the
 * computer, like the tab on a real floppy). */
sd_result_t sd_card_get_read_only(const char *rel, bool *ro);
sd_result_t sd_card_set_read_only(const char *rel, bool ro);

/*
 * Write changed parts of file rel in place (same size, no new clusters):
 * for every bit i in mask, region i (region bytes at i * region, as far
 * as the file goes) from data, which holds the whole new file of size
 * bytes. SD_INVALID: another size; SD_READ_ONLY. Flushed to the card
 * before it returns.
 */
sd_result_t sd_card_update(const char *rel, const uint8_t *data, uint32_t size, uint32_t mask,
                           uint32_t region);

/*
 * Store data (a disk image of size bytes, CRC-32 crc) as a new file next
 * to rel: "<name> (changes).st", or "(changes 2)" ... when that exists.
 * For written sectors whose own file was replaced or deleted meanwhile.
 * out: the new file's path (below the root directory).
 */
sd_result_t sd_card_save_changes(const char *rel, const uint8_t *data, uint32_t size,
                                 uint32_t crc, char *out, size_t out_size);

/* CRC-32 and size of file rel (read in pieces, the card free in between). */
sd_result_t sd_card_file_crc(const char *rel, uint32_t *crc, uint32_t *size);

/* Create folder rel with every folder on the way (SD_EXISTS: it was already there). */
sd_result_t sd_card_mkdir(const char *rel);

/* Remove folder rel (not the root directory): empty, or with everything
 * in it when recursive. */
sd_result_t sd_card_rmdir(const char *rel, bool recursive);

/* Remove file rel (also a read-only one). */
sd_result_t sd_card_remove(const char *rel);

/*
 * Writing a file in pieces: into a hidden temporary file in the target
 * folder; only sd_card_writer_commit (after reading it back and checking
 * the CRC-32) gives it its name, replacing an existing file only when
 * overwrite was set and it is not read-only (SD_READ_ONLY). Anything else
 * leaves no file behind.
 */
typedef struct sd_writer sd_writer_t;
sd_result_t sd_card_writer_open(const char *folder, const char *name, bool overwrite,
                                sd_writer_t **w);
sd_result_t sd_card_writer_write(sd_writer_t *w, const void *data, uint32_t len);
sd_result_t sd_card_writer_commit(sd_writer_t *w, uint32_t crc);     /* frees w */
void sd_card_writer_abort(sd_writer_t *w);                           /* NULL is fine */

/*
 * Read file rel completely into a new PSRAM buffer (*buf, caller frees),
 * *size its length. SD_INVALID also when it is larger than max or a folder.
 */
sd_result_t sd_card_load(const char *rel, uint32_t max, uint8_t **buf, uint32_t *size);

/*
 * Reading a file in pieces without holding the card for the whole file:
 * the lock is taken per piece, so other card users (the web page, the
 * API) are served in between. SD_INVALID also when it is larger than max
 * or a folder. *size: the file's length.
 */
typedef struct sd_reader sd_reader_t;
sd_result_t sd_card_reader_open(const char *rel, uint32_t max, sd_reader_t **r, uint32_t *size);
sd_result_t sd_card_reader_read(sd_reader_t *r, void *buf, uint32_t len, uint32_t *got);
void sd_card_reader_close(sd_reader_t *r);          /* NULL is fine */

const char *sd_result_name(sd_result_t r);
