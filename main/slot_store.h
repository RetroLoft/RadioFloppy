/*
 * RadioFloppy slot store: 20 fixed image slots of 800 KiB on the external
 * SPI flash plus a small catalog (two copies) in the metadata area.
 * Addresses: flash_layout.h. Format: docs/FLASH_LAYOUT.md.
 *
 * Slot indexes are 0..RF_SLOT_COUNT-1; show them to the user as 1..20.
 *
 * Writing an image into a slot:
 *   slot_store_prepare()  -> record BUILDING in the catalog, erase only the
 *                            sectors the new image needs (inside the slot)
 *   slot_store_write()    -> program data (bounds checked)
 *   slot_store_commit()   -> read everything back, check the CRC-32 and
 *                            only then mark the slot VALID
 * An interrupted upload therefore stays BUILDING and is never used.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "flash_layout.h"

#define SLOT_NAME_LEN       40

/* Slot status. 0xFF = erased = never used. */
#define SLOT_EMPTY          0xff
#define SLOT_VALID          0x01
#define SLOT_BUILDING       0x02    /* upload in progress or interrupted */
#define SLOT_DELETED        0x03    /* free; old bytes still in flash */

#define SLOT_FMT_ST         0x01

typedef struct __attribute__((packed)) {
    char name[SLOT_NAME_LEN];   /* NUL terminated */
    uint32_t size;              /* real image length, 1..RF_IMAGE_MAX_SIZE */
    uint32_t crc32;             /* CRC-32 (IEEE, as zlib) over size bytes */
    uint8_t format;             /* SLOT_FMT_* */
    uint8_t status;             /* SLOT_* */
    uint8_t pad[14];            /* 0xFF, reserved */
} slot_record_t;

_Static_assert(sizeof(slot_record_t) == 64, "slot record layout");

typedef enum {
    SLOT_STORE_VALID,           /* valid catalog found */
    SLOT_STORE_BLANK,           /* both catalog sectors erased: not initialised */
    SLOT_STORE_INVALID,         /* catalog sectors hold unknown/corrupt data */
} slot_store_state_t;

/* Read and validate both catalog copies (newest valid one wins). */
slot_store_state_t slot_store_open(void);
slot_store_state_t slot_store_state(void);
uint32_t slot_store_generation(void);

/* Record of a slot, or NULL (bad index / no catalog). */
const slot_record_t *slot_store_record(int slot);

/* true if the slot holds a usable image (VALID and sane size/format). */
bool slot_store_is_valid(int slot);

/* First VALID slot with this name, or -1. */
int slot_store_find_name(const char *name);

/* A slot that may receive a new image (empty, deleted or interrupted), or -1. */
int slot_store_find_free(void);

/* Start writing a new image into a free slot (see header comment). */
esp_err_t slot_store_prepare(int slot, const char *name, uint8_t format,
                             uint32_t size, uint32_t crc32);

/* Program part of the image; offset + len must stay within its size. */
esp_err_t slot_store_write(int slot, uint32_t offset, const void *data, uint32_t len);

/* Verify the whole image against its CRC-32, then mark the slot VALID. */
esp_err_t slot_store_commit(int slot);

/* Free a slot (metadata only; the bytes are erased when it is reused). */
esp_err_t slot_store_delete(int slot);

/* Read a VALID image into buf (record->size bytes) and check its CRC-32. */
esp_err_t slot_store_load(int slot, uint8_t *buf);

/*
 * Take over images from the legacy v1 catalog whose data already sits at
 * a slot start address (no data is moved or erased). Only on a BLANK slot
 * store. Returns the number of migrated images via *migrated.
 */
esp_err_t slot_store_migrate_legacy(int *migrated);

uint32_t slot_store_crc32(uint32_t crc, const void *data, uint32_t len);
