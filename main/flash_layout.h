/*
 * RadioFloppy external flash layout: block-based image library. The only
 * place where addresses and sizes of the external flash are defined; see
 * docs/FLASH_LAYOUT.md.
 *
 * The flash is divided into logical blocks of RF_BLOCK_SIZE_MIN (64 KiB)
 * or a larger power of two, chosen from the detected capacity so that
 * there are at most 256 blocks. Block 0 holds the catalog and settings;
 * blocks 1..data_blocks (at most 255, so a block number fits a uint8_t)
 * hold image data. An image is an ordered list of blocks (not contiguous).
 *
 * Block 0, first 64 KiB (the rest of a larger block 0 is unused):
 *   0x000000  catalog copy A   24 KiB
 *   0x006000  catalog copy B   24 KiB
 *   0x00C000  settings copy A   4 KiB
 *   0x00D000  settings copy B   4 KiB
 *   0x00E000  reserved          8 KiB
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RF_SECTOR_SIZE          0x1000u     /* 4 KiB erase sector */

#define RF_MAX_IMAGE_SIZE       (1536u * 1024u)     /* 1.5 MiB = 1 572 864 bytes */
#define RF_BLOCK_SIZE_MIN       (64u * 1024u)
#define RF_MAX_BLOCKS           256u        /* logical blocks incl. block 0 */
#define RF_MAX_DATA_BLOCKS      255u        /* blocks 1..255: fits uint8_t */
#define RF_MAX_BLOCKS_PER_IMAGE 24          /* 1.5 MiB / 64 KiB */

/* Block 0 */
#define RF_CAT_A                0x000000u
#define RF_CAT_B                0x006000u
#define RF_CAT_SIZE             0x006000u   /* 24 KiB per copy */
#define RF_SETTINGS_A           0x00C000u   /* device settings, two copies */
#define RF_SETTINGS_B           0x00D000u
#define RF_META_RESERVED        0x00E000u   /* .. RF_BLOCK_SIZE_MIN */

/* Formats before the image library (only recognised, never read as new). */
#define RF_OLD_SLOT_CAT_A       0x002000u   /* 20-slot catalog, "RFSL" */
#define RF_OLD_SLOT_CAT_B       0x003000u

_Static_assert(RF_MAX_IMAGE_SIZE / RF_BLOCK_SIZE_MIN == RF_MAX_BLOCKS_PER_IMAGE,
               "1.5 MiB in 64 KiB blocks");
_Static_assert(RF_CAT_B == RF_CAT_A + RF_CAT_SIZE, "catalog copies adjacent");
_Static_assert(RF_CAT_B + RF_CAT_SIZE <= RF_SETTINGS_A, "catalogs before settings");
_Static_assert(RF_META_RESERVED <= RF_BLOCK_SIZE_MIN, "metadata inside block 0");
_Static_assert(RF_CAT_SIZE % RF_SECTOR_SIZE == 0, "catalog is whole sectors");

typedef struct {
    uint32_t capacity;          /* detected flash size in bytes */
    uint32_t block_size;        /* logical block, power of two >= 64 KiB */
    uint16_t data_blocks;       /* usable blocks 1..data_blocks */
    uint8_t max_blocks;         /* blocks for RF_MAX_IMAGE_SIZE (24/12/6/3...) */
} rf_geometry_t;

/*
 * Geometry for a flash of `capacity` bytes: the smallest power-of-two
 * block size >= 64 KiB giving at most 256 blocks. false if the flash is
 * too small (fewer than 2 blocks) or the size is not a whole number of
 * blocks.
 */
static inline bool rf_geometry_for(uint32_t capacity, rf_geometry_t *g)
{
    uint32_t bs = RF_BLOCK_SIZE_MIN;

    while (capacity / bs > RF_MAX_BLOCKS) {
        bs <<= 1;
    }
    uint32_t blocks = capacity / bs;
    if (blocks < 2 || capacity % bs) {
        return false;
    }
    g->capacity = capacity;
    g->block_size = bs;
    g->data_blocks = (uint16_t)(blocks - 1 > RF_MAX_DATA_BLOCKS ? RF_MAX_DATA_BLOCKS : blocks - 1);
    g->max_blocks = (uint8_t)((RF_MAX_IMAGE_SIZE + bs - 1) / bs);
    return true;
}

static inline uint32_t rf_block_addr(const rf_geometry_t *g, uint8_t block)
{
    return (uint32_t)block * g->block_size;
}

static inline uint32_t rf_blocks_for(const rf_geometry_t *g, uint32_t size)
{
    return (size + g->block_size - 1) / g->block_size;
}
