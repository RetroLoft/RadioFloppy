/*
 * Empty formatted Atari disk. See st_format.h.
 *
 * Layout (sectors): 0 boot sector, 1-5 FAT 1, 6-10 FAT 2, 11-17 root
 * directory (112 entries), 18-1439 data (711 clusters of 2 sectors).
 */
#include <string.h>

#include "st_format.h"

#define SECTOR          512
#define SPT             9
#define HEADS           2
#define TRACKS          80
#define TOTAL_SECTORS   (TRACKS * HEADS * SPT)      /* 1440 */
#define FAT_SECTORS     5
#define ROOT_ENTRIES    112
#define ROOT_SECTORS    (ROOT_ENTRIES * 32 / SECTOR)    /* 7 */
#define FIRST_DATA      (1 + 2 * FAT_SECTORS + ROOT_SECTORS)
#define MEDIA           0xf9        /* double sided, 80 tracks, 9 sectors */
#define BOOT_EXEC_SUM   0x1234      /* big-endian word sum of an executable boot sector */

_Static_assert(TOTAL_SECTORS * SECTOR == ST_BLANK_SIZE, "720 KiB");

static void le16(uint8_t *p, unsigned v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static unsigned boot_sum(const uint8_t *b)
{
    unsigned sum = 0;
    for (int i = 0; i < SECTOR; i += 2) {
        sum += (b[i] << 8) | b[i + 1];
    }
    return sum & 0xffff;
}

void st_format_blank(uint8_t *out, uint32_t serial)
{
    memset(out, 0, FIRST_DATA * SECTOR);
    /* Data sectors: the fill byte TOS writes when formatting. */
    memset(out + FIRST_DATA * SECTOR, 0xe5, (TOTAL_SECTORS - FIRST_DATA) * SECTOR);

    uint8_t *b = out;
    b[0] = 0x60;                        /* BRA.S, as TOS writes it */
    b[1] = 0x38;
    memcpy(b + 2, "RFLOPY", 6);         /* OEM / loader bytes */
    b[8] = serial & 0xff;               /* 24-bit serial number */
    b[9] = (serial >> 8) & 0xff;
    b[10] = (serial >> 16) & 0xff;
    le16(b + 11, SECTOR);               /* BPB, little endian */
    b[13] = 2;                          /* sectors per cluster */
    le16(b + 14, 1);                    /* reserved sectors (boot) */
    b[16] = 2;                          /* FATs */
    le16(b + 17, ROOT_ENTRIES);
    le16(b + 19, TOTAL_SECTORS);
    b[21] = MEDIA;
    le16(b + 22, FAT_SECTORS);
    le16(b + 24, SPT);
    le16(b + 26, HEADS);
    le16(b + 28, 0);                    /* hidden sectors */

    /* Never executable: the word sum must not be 0x1234. */
    if (boot_sum(b) == BOOT_EXEC_SUM) {
        b[10] ^= 0x01;
    }

    /* Both FATs: media byte, then FF FF for the two reserved entries. */
    for (int f = 0; f < 2; f++) {
        uint8_t *fat = out + (1 + f * FAT_SECTORS) * SECTOR;
        fat[0] = MEDIA;
        fat[1] = 0xff;
        fat[2] = 0xff;
    }
}
