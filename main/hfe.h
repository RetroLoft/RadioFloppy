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
 *
 * The file is read as a stream (it is never whole in memory): header,
 * track table, then the tracks one cylinder at a time. That needs the
 * track table before the track data and the tracks in ascending order
 * of cylinder, as HxC tools write them; other layouts are refused
 * (HFE_BAD_LAYOUT).
 *
 * Decoded tracks go into a packed track buffer (the player's format):
 * a table of HFE_SLOTS slots, then per track its cells (MSB first,
 * 32-bit aligned), its timing segments and its weak areas. Checking a
 * file runs the same packing without a buffer, so a file that passes
 * the check with the buffer size also fits when it is played.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HFE_MAX_CYLS        84
#define HFE_MAX_SEGMENTS    16      /* bitrate changes per track */
#define HFE_MAX_WEAK        32      /* weak (RAND) areas per track */
#define HFE_MAX_TRACK_CELLS (32767u * 8)   /* one side: 16-bit length / 2 * 8 */
#define HFE_REGION_MAX      65536u  /* track data of one cylinder, both sides */

typedef enum {
    HFE_OK,
    HFE_NOT_HFE,            /* no HFE signature */
    HFE_V2,                 /* HFEv2: not supported */
    HFE_BAD_HEADER,         /* header values inconsistent */
    HFE_UNSUPPORTED,        /* valid, but not for our Atari drive */
    HFE_BAD_TRACKS,         /* track table / track data outside the file */
    HFE_BAD_LAYOUT,         /* valid, but tracks not in stream order */
    HFE_BAD_OPCODE,         /* v3: unknown or malformed opcode */
    HFE_TOO_LARGE,          /* does not fit the track buffer */
} hfe_result_t;

typedef struct {
    uint8_t version;        /* 1 or 3 */
    uint8_t cylinders;
    uint8_t sides;
    uint8_t interface_mode;
    uint16_t bitrate;       /* kbit/s */
    uint16_t lut_block;
    uint32_t cells_total;   /* all tracks, both sides, as played */
    uint32_t bytes_needed;  /* packed track buffer bytes (with the slot table) */
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

/* ---- Packed track buffer --------------------------------------------------- */

#define HFE_SLOTS           (HFE_MAX_CYLS * 2)

/* Slot of cylinder c, side s: HFE_SLOTS entries at the buffer start. */
typedef struct {
    uint32_t offset;        /* of the cells, from the buffer start (0: no track) */
    uint32_t cells;
    uint8_t nseg;           /* segments follow the cells (32-bit aligned) */
    uint8_t nweak;          /* weak areas follow the segments */
    uint16_t reserved;
} hfe_slot_t;

#define HFE_TABLE_BYTES     ((uint32_t)(HFE_SLOTS * sizeof(hfe_slot_t)))

typedef struct {
    uint8_t *buf;           /* NULL: count only (checking a file) */
    uint32_t cap;
    uint32_t used;
} hfe_pack_t;

/* One packed track, for the player. cells NULL: no track (unformatted). */
typedef struct {
    const uint8_t *cells;   /* MSB first; cell 0 follows the index pulse */
    uint32_t count;
    const hfe_segment_t *seg;
    uint8_t nseg;
    const hfe_weak_t *weak;
    uint8_t nweak;
} hfe_packed_track_t;

void hfe_pack_init(hfe_pack_t *p, uint8_t *buf, uint32_t cap);
void hfe_packed_track(const uint8_t *buf, int cyl, int side, hfe_packed_track_t *t);

/* ---- Streaming reader -------------------------------------------------------- */

typedef struct {
    hfe_pack_t *pack;       /* where tracks go (count-only for a check) */
    uint8_t *region;        /* HFE_REGION_MAX bytes: one cylinder */
    uint32_t file_size;
    uint32_t pos;           /* file offset of the next byte */
    uint32_t want;          /* bytes to collect for the current part */
    uint32_t have;
    int phase;
    int cyl;
    uint32_t ns_x16;        /* default cell time */
    hfe_result_t result;    /* sticky */
    hfe_info_t info;
    hfe_track_t trk;        /* scratch for the side being decoded */
    uint8_t hdr[512];
    uint8_t lut[HFE_MAX_CYLS * 4];
} hfe_stream_t;

/*
 * Read a file of file_size bytes that arrives in pieces. region: a buffer
 * of HFE_REGION_MAX bytes. Every track is decoded and packed into pack
 * (hfe_pack_init first; a NULL buffer only counts). Checks everything
 * hfe_check does, as early as possible; after an error, feeding does
 * nothing and returns it again.
 */
void hfe_stream_init(hfe_stream_t *s, uint32_t file_size, uint8_t *region, hfe_pack_t *pack);
hfe_result_t hfe_stream_feed(hfe_stream_t *s, const uint8_t *data, uint32_t len);
/* After the last byte: HFE_OK when the whole file was read and packed. */
hfe_result_t hfe_stream_end(hfe_stream_t *s);
static inline const hfe_info_t *hfe_stream_info(const hfe_stream_t *s) { return &s->info; }

/*
 * Check a whole file in memory (a stream fed at once): signature and
 * version, header values, the track table and its layout, every track
 * (bounds; v3 opcodes decoded and checked) and whether it fits in
 * buffer_bytes of packed track buffer. Never reads outside data[size].
 */
hfe_result_t hfe_check(const uint8_t *data, uint32_t size, uint32_t buffer_bytes,
                       hfe_info_t *info);

/* Recognise an HFE file from its first 8 bytes (any version). */
bool hfe_signature(const uint8_t *data, uint32_t size);

const char *hfe_result_name(hfe_result_t r);
