/*
 * Writing to the emulated floppy (Atari, .ST images set to READ_WRITE).
 *
 * Receive: WDATA is sampled by an RMT RX channel with DMA (0.1 us); one
 * WGATE period (a sector) ends when WDATA stays quiet for 16 us. No GPIO
 * interrupt per pulse.
 * Decode: MFM (sync, data mark, 512 bytes, CRC-16); only a complete,
 * correct data field is used. Which sector: from the rotation position
 * when WGATE was asserted (time since the INDEX pulse), i.e. the ID field
 * that has just passed, on the cylinder and side of that moment.
 * Apply: the sector goes into the kept image in PSRAM, its track is
 * encoded again (reads return the new data at once), its storage block is
 * marked changed.
 * Save: SAVE_DELAY_MS after the last write the changed blocks are stored
 * copy-on-write and switched in with one catalog update
 * (image_store_commit_blocks): after a power cut the image on the flash is
 * the previous or the new saved version, never a mix. Changes not saved
 * yet (at most about SAVE_DELAY_MS plus the save time) are lost then.
 *
 * Write protection is released only when all of this holds: machine
 * ATARI, a library .ST image set to READ_WRITE, its sectors kept in PSRAM,
 * the image library usable, at least as many free blocks as the image has
 * (so every save fits), no unresolved save error, the receiver running.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SAVE_DELAY_MS   2000    /* quiet time before written sectors are saved */

typedef enum {
    WRITE_READ_ONLY,            /* write-protected (see reason) */
    WRITE_WRITABLE,             /* writes accepted, all saved */
    WRITE_PENDING,              /* written sectors not saved yet */
    WRITE_SAVING,
    WRITE_ERROR,                /* saving failed: protected, changes kept in PSRAM */
} write_state_t;

typedef struct {
    write_state_t state;
    bool writable;              /* WPROT released now */
    const char *reason;         /* why protected ("" when writable) */
    char error[96];             /* last save error */
    uint32_t sectors_written;   /* since start-up */
    uint32_t writes_rejected;   /* CRC / sync / position errors */
    uint32_t saves;
    int64_t last_save_us;       /* esp_timer time of the last save, 0 = none */
} write_status_t;

/* Start the receiver and the save task (after flux_stream_init). */
esp_err_t disk_write_init(void);

/* Re-evaluate write protection now (disk changed, setting changed). */
void disk_write_refresh(void);

/* Before the disk may change: protect, then save everything (blocking).
 * ESP_OK: nothing unsaved remains. */
esp_err_t disk_write_flush(void);

/* Unsaved or failed changes? */
bool disk_write_unsaved(void);

void disk_write_status(write_status_t *st);

const char *disk_write_state_name(write_state_t s);
