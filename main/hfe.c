/*
 * HFE disk images. See hfe.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hfe.h"

#define HDR_SIZE        512
#define BLOCK           512
#define HALF            256

/* v3 opcodes as stored in the file (LSB-first bit order). */
#define OP_NOP          0x0f
#define OP_INDEX        0x8f
#define OP_BITRATE      0x4f
#define OP_SKIP         0xcf
#define OP_RAND         0x2f

/* Interface modes that describe a double-density Atari-style drive. */
#define IFM_IBMPC_DD    0x00
#define IFM_ATARI_DD    0x02
#define IFM_GENERIC_DD  0x07

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static uint8_t rev8(uint8_t b)
{
    b = (b & 0xf0) >> 4 | (b & 0x0f) << 4;
    b = (b & 0xcc) >> 2 | (b & 0x33) << 2;
    return (b & 0xaa) >> 1 | (b & 0x55) << 1;
}

const char *hfe_result_name(hfe_result_t r)
{
    switch (r) {
    case HFE_OK:            return "ok";
    case HFE_NOT_HFE:       return "not an HFE file";
    case HFE_V2:            return "HFEv2 is not supported";
    case HFE_BAD_HEADER:    return "invalid header";
    case HFE_UNSUPPORTED:   return "not supported for this drive";
    case HFE_BAD_TRACKS:    return "invalid track table";
    case HFE_BAD_LAYOUT:    return "track layout not supported";
    case HFE_BAD_OPCODE:    return "invalid HFEv3 opcode";
    default:                return "too large";
    }
}

/* Byte i (0..len-1) of one side of a track: 256-byte halves of 512-byte blocks. */
static inline uint8_t side_byte(const uint8_t *track, int side, uint32_t i)
{
    return track[(i / HALF) * BLOCK + side * HALF + (i % HALF)];
}

/* Too many segments in exact timing: walk() must smooth (see hfe.h). */
#define WALK_SMOOTH     (-1)

/* Close the zone of `cells` cells that took `*time` (1/16 ns): its average
 * cell time becomes a segment from `start`. The remainder of the division
 * is carried into the next zone, so the zones add up to the exact time. */
static void close_zone(hfe_track_t *trk, uint32_t start, uint32_t cells, uint64_t *time)
{
    uint32_t ns = (uint32_t)(*time / cells);
    *time -= (uint64_t)ns * cells;
    if (trk->nseg && trk->seg[trk->nseg - 1].ns_x16 == ns) {
        return;                                 /* same as the zone before */
    }
    trk->seg[trk->nseg++] = (hfe_segment_t) { .start = start, .ns_x16 = ns };
}

/*
 * Walk one side of a track: emit cells (out may be NULL: count only),
 * collect opcode effects in trk. zone_cells 0: every bitrate change is a
 * segment (more than HFE_MAX_SEGMENTS: returns WALK_SMOOTH); otherwise the
 * timing is smoothed into zones of zone_cells cells. Returns HFE_OK or
 * HFE_BAD_OPCODE / HFE_TOO_LARGE (more cells than cap_cells).
 */
static int walk(const uint8_t *track, int side, uint32_t len, bool v3,
                uint32_t default_ns_x16, uint8_t *out, uint32_t cap_cells,
                uint32_t zone_cells, hfe_track_t *trk, uint32_t *ops)
{
    uint32_t n = 0;
    uint32_t cur_ns = default_ns_x16;
    int64_t late_index = -1;        /* a repeated INDEX (must be at the track end) */
    uint32_t zone_start = 0;
    uint64_t zone_time = 0;

    memset(trk, 0, sizeof(*trk));
    trk->index_cell = -1;
    if (!zone_cells) {
        trk->nseg = 1;
        trk->seg[0] = (hfe_segment_t) { .start = 0, .ns_x16 = default_ns_x16 };
    }

    for (uint32_t i = 0; i < len;) {
        uint8_t b = side_byte(track, side, i);
        unsigned first = 0;         /* bits of this byte to skip */
        bool weak = false;

        if (v3 && (b & 0x0f) == 0x0f) {
            (*ops)++;
            if (b == OP_NOP) {
                i++;
                continue;
            }
            if (b == OP_INDEX) {
                if (trk->index_cell < 0) {
                    trk->index_cell = (int32_t)n;
                } else if (late_index < 0) {
                    late_index = n;             /* the earliest repeat: checked at the end */
                }
                i++;
                continue;
            }
            if (b == OP_BITRATE) {
                if (i + 1 >= len) {
                    return HFE_BAD_OPCODE;
                }
                uint8_t x = rev8(side_byte(track, side, i + 1));
                /* x: cell rate in units of 1/36 us per cell (72 = 2 us). */
                if (x < 36 || x > 144) {
                    return HFE_BAD_OPCODE;
                }
                uint32_t ns = (uint32_t)x * 16000u * 2 / 72;    /* 1/16 ns */
                if (ns != cur_ns) {
                    if (zone_cells) {
                        /* smoothing: the zones take care of it */
                    } else if (trk->seg[trk->nseg - 1].start == n) {
                        trk->seg[trk->nseg - 1].ns_x16 = ns;    /* no cells in between */
                    } else if (trk->nseg < HFE_MAX_SEGMENTS) {
                        trk->seg[trk->nseg++] = (hfe_segment_t) { .start = n, .ns_x16 = ns };
                    } else {
                        return WALK_SMOOTH;     /* too many bitrate changes */
                    }
                    cur_ns = ns;
                }
                i += 2;
                continue;
            }
            if (b == OP_SKIP) {
                if (i + 2 >= len) {
                    return HFE_BAD_OPCODE;
                }
                first = rev8(side_byte(track, side, i + 1)) & 7;
                i += 2;
                b = side_byte(track, side, i);
            } else if (b == OP_RAND) {
                weak = true;
            } else {
                return HFE_BAD_OPCODE;          /* reserved */
            }
        }

        if (weak) {
            hfe_weak_t *w = trk->nweak ? &trk->weak[trk->nweak - 1] : NULL;
            if (w && w->start + w->count == n) {
                w->count += 8;                  /* adjacent: one area */
            } else if (trk->nweak < HFE_MAX_WEAK - 1) {  /* room for a split at the index */
                trk->weak[trk->nweak++] = (hfe_weak_t) { .start = n, .count = 8 };
            } else {
                w->count = n + 8 - w->start;    /* full: join, the cells between become weak */
            }
            b = 0x44;                           /* placeholder: replaced per revolution */
        }
        for (unsigned k = first; k < 8; k++) {
            if (n >= cap_cells) {
                return HFE_TOO_LARGE;
            }
            if (out) {
                uint8_t bit = (b >> k) & 1;     /* LSB first in the file */
                if (bit) {
                    out[n >> 3] |= 0x80 >> (n & 7);
                } else {
                    out[n >> 3] &= ~(0x80 >> (n & 7));
                }
            }
            n++;
            if (zone_cells) {
                zone_time += cur_ns;
                if (n - zone_start == zone_cells) {
                    close_zone(trk, zone_start, zone_cells, &zone_time);
                    zone_start = n;
                }
            }
        }
        i++;
    }
    if (zone_cells && n > zone_start) {
        close_zone(trk, zone_start, n - zone_start, &zone_time);
    }
    if (late_index >= 0 && (n - (uint32_t)late_index) > n / 64) {
        return HFE_BAD_OPCODE;                  /* a second INDEX within the revolution */
    }
    trk->cells = n;
    return HFE_OK;
}


/* ---- Header ------------------------------------------------------------------ */

bool hfe_signature(const uint8_t *data, uint32_t size)
{
    return size >= 8 && (memcmp(data, "HXCPICFE", 8) == 0 || memcmp(data, "HXCHFEV3", 8) == 0);
}

/* The 512-byte header of a file of `size` bytes. */
static hfe_result_t header(const uint8_t *data, uint32_t size, hfe_info_t *info)
{
    memset(info, 0, sizeof(*info));
    bool v1 = memcmp(data, "HXCPICFE", 8) == 0;
    bool v3 = memcmp(data, "HXCHFEV3", 8) == 0;
    uint8_t revision = data[8];
    if (!v1 && !v3) {
        snprintf(info->detail, sizeof(info->detail), "no HFE signature");
        return HFE_NOT_HFE;
    }
    if (v1 && revision == 1) {
        snprintf(info->detail, sizeof(info->detail),
                 "HFEv2 (HXCPICFE revision 1) is not supported; convert it to HFEv1 or HFEv3");
        return HFE_V2;
    }
    if (revision != 0) {
        snprintf(info->detail, sizeof(info->detail), "unknown HFE revision %u", revision);
        return HFE_BAD_HEADER;
    }
    info->version = v3 ? 3 : 1;
    info->cylinders = data[9];
    info->sides = data[10];
    uint8_t encoding = data[11];
    info->bitrate = le16(data + 12);
    info->interface_mode = data[16];
    info->lut_block = le16(data + 18);
    uint8_t single_step = data[21];

    if (info->cylinders == 0 || info->sides < 1 || info->sides > 2 || info->bitrate == 0 ||
        info->lut_block == 0) {
        snprintf(info->detail, sizeof(info->detail),
                 "header: %u cylinders, %u sides, %u kbit/s, track table at block %u",
                 info->cylinders, info->sides, info->bitrate, info->lut_block);
        return HFE_BAD_HEADER;
    }
    if (encoding != 0) {
        snprintf(info->detail, sizeof(info->detail), "track encoding %u (only ISO MFM)", encoding);
        return HFE_UNSUPPORTED;
    }
    if (info->bitrate < 225 || info->bitrate > 275) {
        snprintf(info->detail, sizeof(info->detail),
                 "%u kbit/s (a double density Atari drive needs 250)", info->bitrate);
        return HFE_UNSUPPORTED;
    }
    if (info->interface_mode != IFM_ATARI_DD && info->interface_mode != IFM_GENERIC_DD &&
        info->interface_mode != IFM_IBMPC_DD) {
        snprintf(info->detail, sizeof(info->detail),
                 "interface mode %u (not a double density ST/Shugart drive)", info->interface_mode);
        return HFE_UNSUPPORTED;
    }
    if (info->cylinders > HFE_MAX_CYLS) {
        snprintf(info->detail, sizeof(info->detail), "%u cylinders (at most %d)",
                 info->cylinders, HFE_MAX_CYLS);
        return HFE_UNSUPPORTED;
    }
    if (single_step != 0xff) {
        snprintf(info->detail, sizeof(info->detail), "double-step image (for 40-track drives)");
        return HFE_UNSUPPORTED;
    }
    if ((uint32_t)info->lut_block * BLOCK + info->cylinders * 4u > size) {
        snprintf(info->detail, sizeof(info->detail), "track table outside the file");
        return HFE_BAD_TRACKS;
    }
    return HFE_OK;
}

/* ---- Rotation to the INDEX opcode ---------------------------------------------- */

/* Cells: in place, by reversal (rev(0,k), rev(k,n), rev(0,n)). */
static void rotate_cells(uint8_t *out, uint32_t k, uint32_t n)
{
    #define BIT(i)      ((out[(i) >> 3] >> (7 - ((i) & 7))) & 1)
    #define SETBIT(i, v) (out[(i) >> 3] = (out[(i) >> 3] & ~(0x80 >> ((i) & 7))) | ((v) << (7 - ((i) & 7))))
    uint32_t spans[3][2] = { { 0, k }, { k, n }, { 0, n } };
    for (int s = 0; s < 3; s++) {
        for (uint32_t a = spans[s][0], b = spans[s][1]; a + 1 < b; a++, b--) {
            unsigned x = BIT(a), y = BIT(b - 1);
            SETBIT(a, y);
            SETBIT(b - 1, x);
        }
    }
    #undef BIT
    #undef SETBIT
}

/* Segments and weak areas move with the cells; old cell k becomes cell 0. */
static hfe_result_t rotate_meta(hfe_track_t *trk, uint32_t k)
{
    uint32_t n = trk->cells;

    /* The segment in force at old cell k starts the track; later ones
     * follow; earlier ones come after the wrap. */
    hfe_segment_t seg[HFE_MAX_SEGMENTS + 1];
    int m = 0, in_force = 0;
    for (int i = 0; i < trk->nseg; i++) {
        if (trk->seg[i].start <= k) {
            in_force = i;
        }
    }
    seg[m++] = (hfe_segment_t) { .start = 0, .ns_x16 = trk->seg[in_force].ns_x16 };
    for (int i = in_force + 1; i < trk->nseg; i++) {
        seg[m++] = (hfe_segment_t) { .start = trk->seg[i].start - k, .ns_x16 = trk->seg[i].ns_x16 };
    }
    for (int i = 0; i <= in_force; i++) {
        if (trk->seg[i].start + n - k < n) {
            seg[m++] = (hfe_segment_t) { .start = trk->seg[i].start + n - k,
                                         .ns_x16 = trk->seg[i].ns_x16 };
        }
    }
    /* Merge neighbours with the same cell time. */
    int u = 0;
    for (int i = 0; i < m; i++) {
        if (u && seg[u - 1].ns_x16 == seg[i].ns_x16) {
            continue;
        }
        seg[u++] = seg[i];
    }
    if (u > HFE_MAX_SEGMENTS) {
        return HFE_BAD_OPCODE;
    }
    memcpy(trk->seg, seg, u * sizeof(seg[0]));
    trk->nseg = (uint8_t)u;

    /* Weak areas likewise; one spanning the rotation point is split. */
    hfe_weak_t weak[HFE_MAX_WEAK + 1];
    int w = 0;
    for (int i = 0; i < trk->nweak; i++) {
        uint32_t st = trk->weak[i].start, cnt = trk->weak[i].count;
        if (st < k && st + cnt > k) {
            weak[w++] = (hfe_weak_t) { .start = 0, .count = st + cnt - k };
            weak[w++] = (hfe_weak_t) { .start = st + n - k, .count = k - st };
        } else {
            weak[w++] = (hfe_weak_t) { .start = (st + n - k) % n, .count = cnt };
        }
    }
    if (w > HFE_MAX_WEAK) {
        return HFE_BAD_OPCODE;
    }
    /* Sorted by start for the player. */
    for (int i = 1; i < w; i++) {
        hfe_weak_t x = weak[i];
        int j = i;
        while (j > 0 && weak[j - 1].start > x.start) {
            weak[j] = weak[j - 1];
            j--;
        }
        weak[j] = x;
    }
    memcpy(trk->weak, weak, w * sizeof(weak[0]));
    trk->nweak = (uint8_t)w;
    trk->index_cell = 0;
    return HFE_OK;
}

/* ---- Packed track buffer --------------------------------------------------- */

static inline uint32_t cell_bytes(uint32_t cells)
{
    return (cells + 31) / 32 * 4;               /* 32-bit aligned */
}

void hfe_pack_init(hfe_pack_t *p, uint8_t *buf, uint32_t cap)
{
    p->buf = buf;
    p->cap = cap;
    p->used = HFE_TABLE_BYTES;
    if (buf && cap >= HFE_TABLE_BYTES) {
        memset(buf, 0, HFE_TABLE_BYTES);        /* every slot: no track */
    }
}

void hfe_packed_track(const uint8_t *buf, int cyl, int side, hfe_packed_track_t *t)
{
    const hfe_slot_t *s = (const hfe_slot_t *)buf + cyl * 2 + side;

    memset(t, 0, sizeof(*t));
    if (cyl < 0 || cyl >= HFE_MAX_CYLS || side < 0 || side > 1 || !s->offset) {
        return;
    }
    t->cells = buf + s->offset;
    t->count = s->cells;
    t->seg = (const hfe_segment_t *)(buf + s->offset + cell_bytes(s->cells));
    t->nseg = s->nseg;
    t->weak = (const hfe_weak_t *)(t->seg + s->nseg);
    t->nweak = s->nweak;
}

/*
 * Decode one side of the cylinder in `region` and pack it. With a buffer
 * the cells are written in place and it must fit now; counting only,
 * `used` may pass `cap` (hfe_stream_end reports the total).
 */
static hfe_result_t pack_side(hfe_pack_t *p, int cyl, int side, const uint8_t *region,
                              uint32_t len, bool v3, uint32_t ns, hfe_track_t *trk,
                              uint32_t *ops, bool *smoothed)
{
    uint32_t at = p->used;
    uint8_t *out = p->buf ? p->buf + at : NULL;
    uint32_t cap_cells = HFE_MAX_TRACK_CELLS;
    if (out) {
        uint32_t room = p->cap > at ? p->cap - at : 0;
        if (room / 4 * 32 < cap_cells) {
            cap_cells = room / 4 * 32;
        }
    }
    uint32_t ops_before = *ops;
    int w = walk(region, side, len, v3, ns, out, cap_cells, 0, trk, ops);
    *smoothed = w == WALK_SMOOTH;
    if (*smoothed) {
        /* Zones of equal length: len * 8 cells at most, and one zone to
         * spare for the split at the index. */
        uint32_t zone = (len * 8 + HFE_MAX_SEGMENTS - 2) / (HFE_MAX_SEGMENTS - 1);
        *ops = ops_before;
        w = walk(region, side, len, v3, ns, out, cap_cells, zone, trk, ops);
    }
    hfe_result_t r = (hfe_result_t)w;
    if (r != HFE_OK) {
        return r;
    }
    if (trk->index_cell > 0) {
        if (out) {
            rotate_cells(out, (uint32_t)trk->index_cell, trk->cells);
        }
        if ((r = rotate_meta(trk, (uint32_t)trk->index_cell)) != HFE_OK) {
            return r;
        }
    }
    uint32_t meta = trk->nseg * sizeof(hfe_segment_t) + trk->nweak * sizeof(hfe_weak_t);
    uint32_t bytes = cell_bytes(trk->cells) + meta;
    if (out) {
        if (at + bytes > p->cap) {
            return HFE_TOO_LARGE;
        }
        uint8_t *m = out + cell_bytes(trk->cells);
        memcpy(m, trk->seg, trk->nseg * sizeof(hfe_segment_t));
        memcpy(m + trk->nseg * sizeof(hfe_segment_t), trk->weak, trk->nweak * sizeof(hfe_weak_t));
        hfe_slot_t *s = (hfe_slot_t *)p->buf + cyl * 2 + side;
        *s = (hfe_slot_t) { .offset = at, .cells = trk->cells, .nseg = trk->nseg,
                            .nweak = trk->nweak };
    }
    p->used = at + bytes;
    return HFE_OK;
}

/* ---- Streaming reader -------------------------------------------------------- */

enum { P_HEADER, P_TABLE, P_TRACK, P_TAIL };

/* Track data of cylinder c from the table: file offset, bytes per side, region bytes. */
static void table_entry(const hfe_stream_t *s, int c, uint32_t *off, uint32_t *len,
                        uint32_t *region)
{
    const uint8_t *e = s->lut + c * 4;
    *off = (uint32_t)le16(e) * BLOCK;
    *len = le16(e + 2) / 2;
    *region = (*len + HALF - 1) / HALF * BLOCK;
}

static hfe_result_t fail(hfe_stream_t *s, hfe_result_t r)
{
    s->result = r;
    return r;
}

void hfe_stream_init(hfe_stream_t *s, uint32_t file_size, uint8_t *region, hfe_pack_t *pack)
{
    memset(s, 0, sizeof(*s));
    s->file_size = file_size;
    s->region = region;
    s->pack = pack;
    s->phase = P_HEADER;
    s->want = HDR_SIZE;
    if (file_size < HDR_SIZE) {
        snprintf(s->info.detail, sizeof(s->info.detail), "file too small for an HFE header");
        s->result = HFE_NOT_HFE;
    }
}

/* The track table is complete: check every entry before any track data. */
static hfe_result_t check_table(hfe_stream_t *s)
{
    hfe_info_t *info = &s->info;
    uint32_t table_end = ((uint32_t)info->lut_block * BLOCK + info->cylinders * 4u + BLOCK - 1) /
                         BLOCK * BLOCK;
    uint32_t prev_end = table_end;

    for (int c = 0; c < info->cylinders; c++) {
        uint32_t off, len, region;
        table_entry(s, c, &off, &len, &region);
        if (len == 0 || off + region > s->file_size) {
            snprintf(info->detail, sizeof(info->detail), "track %d outside the file", c);
            return fail(s, HFE_BAD_TRACKS);
        }
        if (off < table_end) {
            snprintf(info->detail, sizeof(info->detail),
                     "track layout not supported: track %d lies before the end of the track "
                     "table (the tracks must follow the table in ascending order)", c);
            return fail(s, HFE_BAD_LAYOUT);
        }
        if (off < prev_end) {
            snprintf(info->detail, sizeof(info->detail),
                     "track layout not supported: track %d starts inside or before track %d "
                     "(the tracks must follow the table in ascending order)", c, c - 1);
            return fail(s, HFE_BAD_LAYOUT);
        }
        prev_end = off + region;
    }
    return HFE_OK;
}

/* The region of s->cyl is complete: decode and pack its sides. */
static hfe_result_t do_cylinder(hfe_stream_t *s)
{
    hfe_info_t *info = &s->info;
    uint32_t off, len, region;
    hfe_track_t *trk = &s->trk;

    table_entry(s, s->cyl, &off, &len, &region);
    for (int side = 0; side < info->sides; side++) {
        bool smoothed;
        hfe_result_t r = pack_side(s->pack, s->cyl, side, s->region, len, info->version == 3,
                                   s->ns_x16, trk, &info->opcodes, &smoothed);
        if (r != HFE_OK) {
            if (r == HFE_TOO_LARGE) {
                snprintf(info->detail, sizeof(info->detail),
                         "track %d side %d does not fit the track buffer", s->cyl, side);
            } else {
                snprintf(info->detail, sizeof(info->detail), "track %d side %d: %s", s->cyl, side,
                         hfe_result_name(r));
            }
            return fail(s, r);
        }
        if (trk->cells < 1000) {
            snprintf(info->detail, sizeof(info->detail), "track %d side %d has only %lu cells",
                     s->cyl, side, (unsigned long)trk->cells);
            return fail(s, HFE_BAD_TRACKS);
        }
        info->cells_total += trk->cells;
        if (trk->cells > info->max_cells) {
            info->max_cells = trk->cells;
        }
        info->weak_areas += trk->nweak;
        info->bitrate_changes += trk->nseg - 1;
        info->smoothed_tracks += smoothed;
    }
    return HFE_OK;
}

/* A part (header, table or a cylinder) is complete: act on it, set up the next. */
static hfe_result_t part_done(hfe_stream_t *s)
{
    hfe_info_t *info = &s->info;
    hfe_result_t r;
    uint32_t off, len, region;

    switch (s->phase) {
    case P_HEADER:
        if ((r = header(s->hdr, s->file_size, info)) != HFE_OK) {
            return fail(s, r);
        }
        s->ns_x16 = 500u * 16000u / info->bitrate;     /* 1/16 ns per cell */
        s->phase = P_TABLE;
        s->want = info->cylinders * 4u;
        break;
    case P_TABLE:
        if ((r = check_table(s)) != HFE_OK) {
            return r;
        }
        s->phase = P_TRACK;
        s->cyl = 0;
        table_entry(s, 0, &off, &len, &region);
        s->want = region;
        break;
    case P_TRACK:
        if ((r = do_cylinder(s)) != HFE_OK) {
            return r;
        }
        if (++s->cyl == info->cylinders) {
            s->phase = P_TAIL;
            s->want = 0;
        } else {
            table_entry(s, s->cyl, &off, &len, &region);
            s->want = region;
        }
        break;
    }
    s->have = 0;
    return HFE_OK;
}

/* File offset where the current part starts. */
static uint32_t part_start(const hfe_stream_t *s)
{
    uint32_t off, len, region;

    switch (s->phase) {
    case P_HEADER:
        return 0;
    case P_TABLE:
        return (uint32_t)s->info.lut_block * BLOCK;
    case P_TRACK:
        table_entry(s, s->cyl, &off, &len, &region);
        return off;
    default:
        return s->file_size;
    }
}

hfe_result_t hfe_stream_feed(hfe_stream_t *s, const uint8_t *data, uint32_t len)
{
    if (s->result != HFE_OK) {
        return s->result;
    }
    if (len > s->file_size - s->pos) {
        snprintf(s->info.detail, sizeof(s->info.detail), "more data than the %lu bytes announced",
                 (unsigned long)s->file_size);
        return fail(s, HFE_BAD_TRACKS);
    }
    while (len) {
        if (s->phase == P_TAIL) {
            s->pos += len;                      /* after the last track: ignored */
            return HFE_OK;
        }
        uint32_t start = part_start(s);
        if (s->pos < start) {
            uint32_t n = start - s->pos < len ? start - s->pos : len;
            s->pos += n;                        /* between parts: skipped */
            data += n;
            len -= n;
            continue;
        }
        uint8_t *dst = s->phase == P_HEADER ? s->hdr : s->phase == P_TABLE ? s->lut : s->region;
        uint32_t n = s->want - s->have < len ? s->want - s->have : len;
        memcpy(dst + s->have, data, n);
        s->have += n;
        s->pos += n;
        data += n;
        len -= n;
        if (s->have == s->want) {
            hfe_result_t r = part_done(s);
            if (r != HFE_OK) {
                return r;
            }
        }
    }
    return HFE_OK;
}

hfe_result_t hfe_stream_end(hfe_stream_t *s)
{
    hfe_info_t *info = &s->info;

    if (s->result != HFE_OK) {
        return s->result;
    }
    if (s->pos != s->file_size || s->phase != P_TAIL) {
        snprintf(info->detail, sizeof(info->detail), "file incomplete: %lu of %lu bytes",
                 (unsigned long)s->pos, (unsigned long)s->file_size);
        return fail(s, HFE_BAD_TRACKS);
    }
    info->bytes_needed = s->pack->used;
    if (info->bytes_needed > s->pack->cap) {
        snprintf(info->detail, sizeof(info->detail),
                 "its tracks need %lu KiB of track buffer, %lu KiB available",
                 (unsigned long)(info->bytes_needed / 1024), (unsigned long)(s->pack->cap / 1024));
        return fail(s, HFE_TOO_LARGE);
    }
    int n = snprintf(info->detail, sizeof(info->detail), "HFEv%u, %u cylinders, %u side(s), %u kbit/s%s",
                     info->version, info->cylinders, info->sides, info->bitrate,
                     info->opcodes ? ", with opcodes" : "");
    if (info->smoothed_tracks) {
        snprintf(info->detail + n, sizeof(info->detail) - n, ", timing smoothed on %u tracks",
                 info->smoothed_tracks);
    }
    return HFE_OK;
}

hfe_result_t hfe_check(const uint8_t *data, uint32_t size, uint32_t buffer_bytes,
                       hfe_info_t *info)
{
    hfe_pack_t pack;
    hfe_stream_t *s = malloc(sizeof(*s));
    uint8_t *region = malloc(HFE_REGION_MAX);

    if (!s || !region) {
        free(s);
        free(region);
        memset(info, 0, sizeof(*info));
        snprintf(info->detail, sizeof(info->detail), "no memory to check the file");
        return HFE_TOO_LARGE;
    }
    hfe_pack_init(&pack, NULL, buffer_bytes);
    hfe_stream_init(s, size, region, &pack);
    hfe_stream_feed(s, data, size);
    hfe_result_t r = hfe_stream_end(s);
    *info = s->info;
    free(region);
    free(s);
    return r;
}
