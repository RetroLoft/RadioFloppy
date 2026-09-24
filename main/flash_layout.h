/*
 * RadioFloppy external SPI flash map (U2, 16 MiB). The only place where
 * addresses and sizes of the external flash are defined; see
 * docs/FLASH_LAYOUT.md.
 *
 *   0x000000  metadata area, 64 KiB (16 x 4 KiB sectors)
 *     0x000000  legacy v1 catalog copy A   (read-only, until migrated)
 *     0x001000  legacy v1 catalog copy B   (read-only, until migrated)
 *     0x002000  slot catalog copy A
 *     0x003000  slot catalog copy B
 *     0x004000  reserved (settings, future metadata) up to 0x00FFFF
 *   0x010000  image slots 1..20, 800 KiB each
 *   0xFB0000  reserved, 320 KiB, not used
 *
 * Slot numbers: index 0..19 internally, shown to the user as 1..20.
 */
#pragma once

#include <stdint.h>

#define RF_FLASH_SIZE           0x1000000u  /* 16 MiB external flash */
#define RF_SECTOR_SIZE          0x1000u     /* 4 KiB erase sector */

/* Metadata area */
#define RF_META_BASE            0x000000u
#define RF_META_SIZE            0x010000u
#define RF_LEGACY_CAT_A         0x000000u   /* v1 variable-length catalog */
#define RF_LEGACY_CAT_B         0x001000u
#define RF_SLOT_CAT_A           0x002000u
#define RF_SLOT_CAT_B           0x003000u
#define RF_META_RESERVED        0x004000u   /* .. RF_META_BASE + RF_META_SIZE */

/* Image slots */
#define RF_SLOT_COUNT           20
#define RF_SLOT_BASE            0x010000u
#define RF_SLOT_SIZE            0x0C8000u   /* 800 KiB = 819200 bytes */
#define RF_SLOT_SECTORS         (RF_SLOT_SIZE / RF_SECTOR_SIZE)
#define RF_SLOTS_END            (RF_SLOT_BASE + RF_SLOT_COUNT * RF_SLOT_SIZE)
#define RF_IMAGE_MAX_SIZE       RF_SLOT_SIZE

/* Unused tail */
#define RF_RESERVED_BASE        RF_SLOTS_END
#define RF_RESERVED_SIZE        (RF_FLASH_SIZE - RF_RESERVED_BASE)

_Static_assert(RF_SLOT_BASE == RF_META_BASE + RF_META_SIZE, "slots follow metadata");
_Static_assert(RF_SLOT_SIZE % RF_SECTOR_SIZE == 0, "slot is whole sectors");
_Static_assert(RF_SLOT_SECTORS == 200, "800 KiB = 200 sectors");
_Static_assert(RF_SLOT_BASE % RF_SECTOR_SIZE == 0, "slots sector aligned");
_Static_assert(RF_SLOTS_END == 0xFB0000u, "slot 20 ends at 0xFAFFFF");
_Static_assert(RF_RESERVED_SIZE == 320 * 1024, "320 KiB reserved tail");
_Static_assert(RF_SLOT_CAT_B + RF_SECTOR_SIZE <= RF_META_RESERVED, "catalogs in metadata");

/* Start address of slot index 0..RF_SLOT_COUNT-1. */
static inline uint32_t rf_slot_start(int slot)
{
    return RF_SLOT_BASE + (uint32_t)slot * RF_SLOT_SIZE;
}

/* Bytes to erase for an image: whole sectors, never more than one slot. */
static inline uint32_t rf_erase_len(uint32_t image_size)
{
    return (image_size + RF_SECTOR_SIZE - 1) / RF_SECTOR_SIZE * RF_SECTOR_SIZE;
}
