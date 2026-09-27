/*
 * A new, empty, formatted Atari disk image (plain C, also built in the host
 * tests). One layout only: .ST, 720 KiB, 80 tracks x 2 sides x 9 sectors
 * x 512 bytes, as TOS formats a double-sided disk.
 */
#pragma once

#include <stdint.h>

#define ST_BLANK_SIZE   737280u     /* 80 x 2 x 9 x 512 */

/*
 * Fill out[ST_BLANK_SIZE] with an empty FAT12 file system: boot sector
 * with BPB (not executable), two empty FATs, an empty root directory and
 * unused data sectors. serial: the 24-bit disk serial number (TOS uses it
 * to notice disk changes; make it differ between disks).
 */
void st_format_blank(uint8_t *out, uint32_t serial);
