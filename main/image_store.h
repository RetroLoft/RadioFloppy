/*
 * RadioFloppy image store on the external SPI flash, format version 1.
 * Full description: docs/IMAGE_STORE.md.
 *
 *   0x000000  catalog copy A (4 KiB)
 *   0x001000  catalog copy B (4 KiB)
 *   0x002000  reserved (metadata / settings)
 *   0x010000  images, variable length, 4 KiB aligned
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define RF_CAT_ADDR_A       0x000000
#define RF_CAT_ADDR_B       0x001000
#define RF_IMAGE_BASE       0x010000
#define RF_ALIGN            4096

#define RF_CAT_MAGIC        0x54434652  /* "RFCT" little endian */
#define RF_CAT_VERSION      1
#define RF_CAT_COMMIT       0x21544d43  /* "CMT!", written last */
#define RF_CAT_RECORDS      60
#define RF_NAME_LEN         40

/* Record status. 0xFF = erased = unused record. */
#define RF_ST_EMPTY         0xff
#define RF_ST_VALID         0x01
#define RF_ST_BUILDING      0x02        /* being written, not usable */
#define RF_ST_DELETED       0x03        /* space may be reused after erase */

#define RF_FMT_ST           0x01

typedef struct __attribute__((packed)) {
    char name[RF_NAME_LEN];     /* NUL terminated */
    uint32_t start;             /* flash address, RF_ALIGN aligned */
    uint32_t size;              /* real image length in bytes */
    uint32_t reserved;          /* allocated length, multiple of RF_ALIGN */
    uint32_t crc32;             /* CRC-32 (IEEE) over the size bytes */
    uint8_t format;             /* RF_FMT_* */
    uint8_t status;             /* RF_ST_* */
    uint8_t pad[6];             /* 0xFF */
} rf_record_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;       /* sizeof(rf_cat_header_t) */
    uint32_t generation;        /* higher = newer */
    uint16_t record_size;       /* sizeof(rf_record_t) */
    uint16_t record_count;      /* RF_CAT_RECORDS */
    uint32_t crc32;             /* over header + records, this field = 0 */
    uint8_t pad[12];            /* 0xFF */
} rf_cat_header_t;

_Static_assert(sizeof(rf_record_t) == 64, "record layout");
_Static_assert(sizeof(rf_cat_header_t) == 32, "header layout");

typedef struct {
    rf_cat_header_t hdr;
    rf_record_t rec[RF_CAT_RECORDS];
} rf_catalog_t;

/* Load the newest valid catalog. ESP_ERR_NOT_FOUND: none (empty store). */
esp_err_t image_store_open(void);

/* Current catalog (after image_store_open), or NULL if there is none. */
const rf_catalog_t *image_store_catalog(void);

/* Find a VALID image by name. */
const rf_record_t *image_store_find(const char *name);

/*
 * Add an image: allocate after the last used area, mark BUILDING, erase
 * only its own sectors, program, verify byte for byte, then mark VALID.
 * Refuses to overwrite unknown (non-blank) flash content when no valid
 * catalog exists.
 */
esp_err_t image_store_add(const char *name, uint8_t format,
                          const uint8_t *data, uint32_t size);

/* Read an image completely into buf and check its CRC-32. */
esp_err_t image_store_load(const rf_record_t *rec, uint8_t *buf);

uint32_t image_store_crc32(const uint8_t *data, uint32_t size);
