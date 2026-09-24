/*
 * Legacy v1 image catalog ("RFCT", variable-length images) as written by
 * the first external-flash firmware, at 0x000000 / 0x001000. Read-only:
 * only used to find images that still have to be migrated to the slot
 * catalog (slot_store.h). Format: docs/FLASH_LAYOUT.md, "Legacy v1".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LEGACY_RECORDS      60
#define LEGACY_NAME_LEN     40
#define LEGACY_ST_VALID     0x01

typedef struct __attribute__((packed)) {
    char name[LEGACY_NAME_LEN];
    uint32_t start;
    uint32_t size;
    uint32_t reserved;
    uint32_t crc32;
    uint8_t format;
    uint8_t status;
    uint8_t pad[6];
} legacy_record_t;

/* Read both copies; true if a valid v1 catalog exists. */
bool legacy_catalog_open(void);

/* Record i (0..LEGACY_RECORDS-1) of the newest valid copy, or NULL. */
const legacy_record_t *legacy_catalog_record(int i);

/* Generation of the newest valid copy (0 if none). */
uint32_t legacy_catalog_generation(void);
