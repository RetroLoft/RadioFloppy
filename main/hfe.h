/*
 * HFE disk images (HxC Floppy Emulator), plain C, also built in the host
 * tests. References: the HxC specification (hxc2001.com, HFE file format)
 * and FlashFloppy src/image/hfe.c (public domain).
 *
 * File layout: 512-byte header, a track table at track_list_offset*512
 * (per cylinder: offset in 512-byte blocks, length in bytes for both
 * sides), track data in 512-byte blocks of which the first 256 bytes
 * belong to side 0 and the next 256 to side 1. Every bit is one bitcell,
 * sent LSB first; the cell time follows from the header bitrate
 * (500 / bitrate us: 2 us at 250 kbit/s MFM).
 *
 * HFEv3 ("HXCHFEV3") adds byte-aligned opcodes: a byte whose low nibble
 * (in file bit order) is 0xF. In file bytes: 0x0F NOP, 0x8F INDEX,
 * 0x4F BITRATE + byte, 0xCF SKIP + byte (0-7 bits of the next byte are
 * skipped), 0x2F RAND (8 random cells, weak bits). The argument bytes are
 * stored bit-reversed. Opcodes are not cells (except RAND, which stands
 * for 8 cells). HFEv2 ("HXCPICFE" revision 1, 4-byte opcodes) is refused.
 *
 * Supported for the Atari profile: ISO MFM, 250 kbit/s nominal, 1 or 2
 * sides, at most 84 cylinders, single step.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HFE_MAX_CYLS        84
#define HFE_MAX_SEGMENTS    16      /* bitrate changes per track */
#define HFE_MAX_WEAK        32      /* weak (RAND) areas per track */
#define HFE_MAX_TRACK_CELLS (32767u * 8)   /* one side: 16-bit length / 2 * 8 */

typedef enum {
    HFE_OK,
    HFE_NOT_HFE,            /* no HFE signature */
    HFE_V2,                 /* HFEv2: not supported */
    HFE_BAD_HEADER,         /* header values inconsistent */
    HFE_UNSUPPORTED,        /* valid, but not for our Atari drive */
    HFE_BAD_TRACKS,         /* track table / track data outside the file */
    HFE_BAD_OPCODE,         /* v3: unknown or malformed opcode */
    HFE_TOO_LARGE,          /* does not fit the track buffers */
} hfe_result_t;

typedef struct {
    uint8_t version;        /* 1 or 3 */
    uint8_t cylinders;
    uint8_t sides;
    uint8_t interface_mode;
    uint16_t bitrate;       /* kbit/s */
    uint16_t lut_block;
    uint32_t cells_total;   /* all tracks, both sides, as played */
    uint32_t bytes_needed;  /* packed track buffer bytes for all tracks */
    uint32_t max_cells;     /* longest track */
    uint16_t opcodes;       /* v3 opcodes seen (all tracks) */
    uint16_t weak_areas;
    uint16_t bitrate_changes;
    char detail[160];       /* human readable reason / remark */
} hfe_info_t;

/* One timing segment: from cell `start` on, each cell lasts `ns_x16`
 * sixteenths of a nanosecond (2 us = 32000). */
typedef struct {
    uint32_t start;
    uint32_t ns_x16;
} hfe_segment_t;

typedef struct {
    uint32_t start;         /* first weak cell */
    uint32_t count;         /* number of cells */
} hfe_weak_t;

typedef struct {
    uint32_t cells;         /* cells in this track (one side) */
    int32_t index_cell;     /* INDEX opcode position, -1 = track start */
    uint8_t nseg;
    hfe_segment_t seg[HFE_MAX_SEGMENTS];
    uint8_t nweak;
    hfe_weak_t weak[HFE_MAX_WEAK];
} hfe_track_t;

/*
 * Check a whole file: signature and version, header values, the track
 * table, every track (bounds; v3 opcodes decoded and checked) and whether
 * it fits in `buffer_bytes` of packed track buffer. Never reads outside
 * data[size].
 */
hfe_result_t hfe_check(const uint8_t *data, uint32_t size, uint32_t buffer_bytes,
                       hfe_info_t *info);

/*
 * Decode one track (cylinder cyl, side 0/1) of a checked file into cells:
 * out gets them packed MSB first (cell 0 = bit 7 of out[0]), at most
 * cap_bytes. Opcodes are removed; the track is rotated so that a v3 INDEX
 * opcode lands on cell 0. Side 1 of a single-sided file gives 0 cells.
 */
hfe_result_t hfe_decode_track(const uint8_t *data, uint32_t size, const hfe_info_t *info,
                              int cyl, int side, uint8_t *out, uint32_t cap_bytes,
                              hfe_track_t *trk);

const char *hfe_result_name(hfe_result_t r);
