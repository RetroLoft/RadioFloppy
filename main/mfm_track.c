/*
 * IBM/ISO MFM track generation for Atari ST DD disks.
 * See mfm_track.h. Reference: FlashFloppy src/image/img.c (mfm_prep_track,
 * mfm_read_track) and src/image/mfm.c.
 */
#include <string.h>

#include "mfm_track.h"

#define GAP_4A      80      /* post-index */
#define GAP_SYNC    12      /* 0x00 bytes before each sync */
#define GAP_2       22      /* post-IDAM */
/* Bytes per sector without GAP3: ID field + GAP2 + data field + CRC. */
#define SECTOR_OVERHEAD (GAP_SYNC + 8 + 2 + GAP_2 + GAP_SYNC + 4 + MFM_SECTOR_SIZE + 2)
#define SYNC_A1     0x4489  /* 0xA1 with missing clock bit */
#define MFM_DAM_CRC 0xe295  /* CRC of A1 A1 A1 FB */

uint16_t mfm_crc16(const uint8_t *buf, int len, uint16_t crc)
{
    /* CRC-16-CCITT, polynomial 0x1021, MSB first. */
    while (len--) {
        crc ^= (uint16_t)*buf++ << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

/* ---- Encoder ------------------------------------------------------------ */

mfm_layout_t mfm_layout(int sectors)
{
    /* FlashFloppy img_type[]: GAP3 and interleave per sector count. */
    mfm_layout_t l = {
        .sectors = sectors,
        .gap3 = sectors <= 9 ? 84 : sectors == 10 ? 30 : 3,
        .interleave = sectors >= 11 ? 2 : 1,
    };
    uint32_t bytes = GAP_4A + sectors * (SECTOR_OVERHEAD + l.gap3);
    uint32_t cells = bytes * 16;
    if (cells < MFM_TRACK_CELLS) {
        cells = MFM_TRACK_CELLS;        /* pre-index gap fills the rest */
    }
    l.cells = (cells + 31) & ~31u;      /* as FlashFloppy: multiple of 32 */
    return l;
}

typedef struct {
    uint8_t *raw;
    int pos;            /* raw bytes written (2 per data byte) */
    unsigned prev;      /* last data bit written */
} writer_t;

static void emit_raw(writer_t *w, uint16_t word)
{
    /* The first cell is a clock cell: no transition after a data 1. */
    if (w->prev) {
        word &= 0x7fff;
    }
    w->raw[w->pos++] = word >> 8;
    w->raw[w->pos++] = word & 0xff;
    w->prev = word & 1;
}

/*
 * MFM word of each byte value, with the first clock bit computed as if the
 * previous data bit were 0; emit_raw() clears it after a data 1. Same
 * approach as FlashFloppy's mfmtab[].
 */
static uint16_t mfm_table[256];

static void init_table(void)
{
    for (int b = 0; b < 256; b++) {
        uint16_t word = 0;
        unsigned prev = 0;
        for (int i = 7; i >= 0; i--) {
            unsigned bit = (b >> i) & 1;
            unsigned clk = !prev && !bit;       /* clock only between two 0 bits */
            word = (word << 2) | (clk << 1) | bit;
            prev = bit;
        }
        mfm_table[b] = word;
    }
}

static void emit_byte(writer_t *w, uint8_t b)
{
    emit_raw(w, mfm_table[b]);
}

static void emit_repeat(writer_t *w, uint8_t b, int n)
{
    while (n--) {
        emit_byte(w, b);
    }
}

/* Physical order of the sector numbers on a track (FlashFloppy
 * raw_seek_track: interleave, optional skew). */
static void sector_order(uint8_t *order, const mfm_layout_t *l, uint8_t cyl, uint8_t head)
{
    int n = l->sectors;
    unsigned pos = 0;

#if MFM_USE_TOS_SKEW
    pos = (cyl * 4 + head * 2) % n;
#else
    (void)cyl;
    (void)head;
#endif
    for (int i = 0; i < n; i++) {
        order[i] = 0;
    }
    for (int i = 0; i < n; i++) {
        while (order[pos]) {
            pos = (pos + 1) % n;
        }
        order[pos] = i + 1;
        pos = (pos + l->interleave) % n;
    }
}

void mfm_build_track(uint8_t *raw, const mfm_layout_t *l, const uint8_t *sectors,
                     uint8_t cyl, uint8_t head)
{
    if (mfm_table[0] == 0) {
        init_table();       /* 0x00 encodes to 0xAAAA, never 0 once built */
    }
    /* The track is a ring: it ends with 0x4E (last data bit 0). */
    writer_t w = { .raw = raw, .pos = 0, .prev = 0 };
    uint8_t order[MFM_MAX_SECTORS];

    sector_order(order, l, cyl, head);

    emit_repeat(&w, 0x4e, GAP_4A);      /* no IAM on ST disks (FlashFloppy) */

    for (int s = 0; s < l->sectors; s++) {
        uint8_t r = order[s];
        const uint8_t *data = sectors + (r - 1) * MFM_SECTOR_SIZE;

        /* ID field */
        uint8_t idam[8] = { 0xa1, 0xa1, 0xa1, 0xfe, cyl, head, r, 2 };
        uint16_t crc = mfm_crc16(idam, sizeof(idam), 0xffff);
        emit_repeat(&w, 0x00, GAP_SYNC);
        for (int i = 0; i < 3; i++) {
            emit_raw(&w, SYNC_A1);
        }
        for (int i = 3; i < 8; i++) {
            emit_byte(&w, idam[i]);
        }
        emit_byte(&w, crc >> 8);
        emit_byte(&w, crc & 0xff);
        emit_repeat(&w, 0x4e, GAP_2);

        /* Data field */
        emit_repeat(&w, 0x00, GAP_SYNC);
        for (int i = 0; i < 3; i++) {
            emit_raw(&w, SYNC_A1);
        }
        emit_byte(&w, 0xfb);
        for (int i = 0; i < MFM_SECTOR_SIZE; i++) {
            emit_byte(&w, data[i]);
        }
        crc = mfm_crc16(data, MFM_SECTOR_SIZE, MFM_DAM_CRC);
        emit_byte(&w, crc >> 8);
        emit_byte(&w, crc & 0xff);
        emit_repeat(&w, 0x4e, l->gap3);
    }

    /* Pre-index gap up to the track length (FlashFloppy: gap_4). */
    while (w.pos < (int)(l->cells / 8)) {
        emit_byte(&w, 0x4e);
    }
}

void mfm_build_blank_track(uint8_t *raw, uint32_t cells)
{
    if (mfm_table[0] == 0) {
        init_table();
    }
    writer_t w = { .raw = raw, .pos = 0, .prev = 0 };

    while (w.pos < (int)(cells / 8)) {
        emit_byte(&w, 0x4e);
    }
}

/* ---- Verifier (independent decoder) ------------------------------------ */

static int track_cells;     /* length of the track being verified */

static inline unsigned cell(const uint8_t *raw, int i)
{
    if (i >= track_cells) {
        i -= track_cells;
    }
    return (raw[i >> 3] >> (7 - (i & 7))) & 1;
}

/* Read the data byte whose 16 cells start at cell index i. */
static uint8_t read_byte(const uint8_t *raw, int i)
{
    uint8_t b = 0;
    for (int k = 0; k < 8; k++) {
        b = (b << 1) | cell(raw, i + 2 * k + 1);
    }
    return b;
}

static bool is_sync(const uint8_t *raw, int i)
{
    uint16_t w = 0;
    for (int k = 0; k < 16; k++) {
        w = (w << 1) | cell(raw, i + k);
    }
    return w == SYNC_A1;
}

int mfm_verify_track(const uint8_t *raw, const mfm_layout_t *l, const uint8_t *sectors,
                     uint8_t cyl, uint8_t head)
{
    int good = 0;
    int seen[MFM_MAX_SECTORS + 1] = { 0 };

    track_cells = l->cells;
    uint8_t buf[4 + MFM_SECTOR_SIZE + 2];

    /* Also reject adjacent transitions (invalid MFM). */
    for (int i = 0; i < track_cells; i++) {
        if (cell(raw, i) && cell(raw, i + 1)) {
            return -1;
        }
    }

    /* Rolling 16-cell window; only on a first sync the next two are checked. */
    uint16_t window = 0;
    for (int i = 0; i < track_cells + 16; i++) {
        window = (window << 1) | cell(raw, i);
        if (window != SYNC_A1) {
            continue;
        }
        int start = i - 15;
        if (!(is_sync(raw, start + 16) && is_sync(raw, start + 32))) {
            continue;
        }
        i = start;
        int p = i + 48;
        uint8_t mark = read_byte(raw, p);
        if (mark != 0xfe) {
            i = p;
            window = 0;
            continue;
        }
        uint8_t id[10] = { 0xa1, 0xa1, 0xa1, 0xfe };
        for (int k = 4; k < 10; k++) {
            id[k] = read_byte(raw, p + 16 * (k - 3));
        }
        if (mfm_crc16(id, 10, 0xffff) != 0 || id[4] != cyl || id[5] != head ||
            id[6] < 1 || id[6] > l->sectors || id[7] != 2) {
            return -2;
        }
        uint8_t r = id[6];

        /* Find the data mark within the next ~60 bytes. */
        int q = p + 16 * 7;
        int end = q + 16 * 60;
        for (; q < end; q++) {
            if (is_sync(raw, q) && is_sync(raw, q + 16) && is_sync(raw, q + 32)) {
                break;
            }
        }
        if (q >= end || read_byte(raw, q + 48) != 0xfb) {
            return -3;
        }
        buf[0] = buf[1] = buf[2] = 0xa1;
        buf[3] = 0xfb;
        for (int k = 0; k < MFM_SECTOR_SIZE + 2; k++) {
            buf[4 + k] = read_byte(raw, q + 64 + 16 * k);
        }
        if (mfm_crc16(buf, sizeof(buf), 0xffff) != 0 ||
            memcmp(buf + 4, sectors + (r - 1) * MFM_SECTOR_SIZE, MFM_SECTOR_SIZE) != 0) {
            return -4;
        }
        if (!seen[r]++) {
            good++;
        }
        i = q + 64 + 16 * (MFM_SECTOR_SIZE + 2);
        window = 0;
    }
    return good;
}
