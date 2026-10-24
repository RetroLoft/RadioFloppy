/*
 * RadioFloppy external flash layout. The only place where addresses and
 * sizes of the external flash are defined; see docs/FLASH_LAYOUT.md.
 *
 * Images live on the SD card. The flash only holds the settings and one
 * copy of the last disk from the card (so it also starts without the card):
 *
 *   0x000000  unused (the old image library's catalogs; erased once)   48 KiB
 *   0x00C000  settings copy A                                            4 KiB
 *   0x00D000  settings copy B                                            4 KiB
 *   0x00E000  unused                                                     8 KiB
 *   0x010000  disk copy: header                                          4 KiB
 *   0x020000  disk copy: data, up to RF_COPY_MAX (64 KiB aligned:        3 MiB
 *             erased in 64 KiB blocks, much faster than 4 KiB sectors)
 *   0x320000  end of what is used (fits a 4 MiB flash)
 */
#pragma once

#include <stdint.h>

#define RF_SECTOR_SIZE          0x1000u     /* 4 KiB erase sector */

#define RF_OLD_CATALOGS         0x000000u   /* old image library: catalog copies A and B */
#define RF_OLD_CATALOGS_SIZE    0x00C000u
#define RF_SETTINGS_A           0x00C000u   /* device settings, two copies */
#define RF_SETTINGS_B           0x00D000u
#define RF_COPY_HEADER          0x010000u   /* disk copy (disk_copy.h) */
#define RF_COPY_DATA            0x020000u
#define RF_COPY_BLOCK           0x010000u   /* erase unit of the copy's data */
#define RF_COPY_MAX             (3u * 1024u * 1024u)
#define RF_USED_END             (RF_COPY_DATA + RF_COPY_MAX)

_Static_assert(RF_SETTINGS_A >= RF_OLD_CATALOGS + RF_OLD_CATALOGS_SIZE, "settings after the old catalogs");
_Static_assert(RF_COPY_HEADER > RF_SETTINGS_B, "copy after the settings");
_Static_assert(RF_USED_END <= 4u * 1024u * 1024u, "fits a 4 MiB flash");
_Static_assert(RF_COPY_DATA % RF_COPY_BLOCK == 0, "copy data block aligned");
