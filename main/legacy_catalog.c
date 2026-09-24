/*
 * Legacy v1 image catalog, read-only. See legacy_catalog.h.
 */
#include <stddef.h>
#include <string.h>

#include "esp_rom_crc.h"

#include "ext_flash.h"
#include "flash_layout.h"
#include "legacy_catalog.h"

#define V1_MAGIC        0x54434652  /* "RFCT" */
#define V1_VERSION      1
#define V1_COMMIT       0x21544d43  /* "CMT!" at the end of the sector */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t generation;
    uint16_t record_size;
    uint16_t record_count;
    uint32_t crc32;
    uint8_t pad[12];
} v1_header_t;

typedef struct {
    v1_header_t hdr;
    legacy_record_t rec[LEGACY_RECORDS];
} v1_catalog_t;

_Static_assert(sizeof(legacy_record_t) == 64, "v1 record layout");
_Static_assert(sizeof(v1_header_t) == 32, "v1 header layout");

static v1_catalog_t current;
static v1_catalog_t other;
static bool valid;

static uint32_t v1_crc(const v1_catalog_t *cat)
{
    static const uint8_t zero[4] = { 0 };
    const uint8_t *p = (const uint8_t *)cat;
    const size_t off = offsetof(v1_header_t, crc32);

    uint32_t crc = esp_rom_crc32_le(0, p, off);
    crc = esp_rom_crc32_le(crc, zero, sizeof(zero));
    return esp_rom_crc32_le(crc, p + off + 4, sizeof(*cat) - off - 4);
}

static bool read_v1(uint32_t addr, v1_catalog_t *cat)
{
    uint32_t commit = 0;

    if (ext_flash_read(addr, cat, sizeof(*cat)) != ESP_OK ||
        ext_flash_read(addr + RF_SECTOR_SIZE - 4, &commit, sizeof(commit)) != ESP_OK) {
        return false;
    }
    return cat->hdr.magic == V1_MAGIC && cat->hdr.version == V1_VERSION &&
           cat->hdr.header_size == sizeof(v1_header_t) &&
           cat->hdr.record_size == sizeof(legacy_record_t) &&
           cat->hdr.record_count == LEGACY_RECORDS &&
           cat->hdr.crc32 == v1_crc(cat) && commit == V1_COMMIT;
}

bool legacy_catalog_open(void)
{
    bool va = read_v1(RF_LEGACY_CAT_A, &current);
    bool vb = read_v1(RF_LEGACY_CAT_B, &other);

    if (vb && (!va || other.hdr.generation > current.hdr.generation)) {
        current = other;
    }
    valid = va || vb;
    return valid;
}

const legacy_record_t *legacy_catalog_record(int i)
{
    return (valid && i >= 0 && i < LEGACY_RECORDS) ? &current.rec[i] : NULL;
}

uint32_t legacy_catalog_generation(void)
{
    return valid ? current.hdr.generation : 0;
}
