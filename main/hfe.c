/*
 * HFE disk images. See hfe.h.
 */
#include <stdio.h>
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
    case HFE_BAD_OPCODE:    return "invalid HFEv3 opcode";
    default:                return "too large";
    }
}

/* Byte i (0..len-1) of one side of a track: 256-byte halves of 512-byte blocks. */
static inline uint8_t side_byte(const uint8_t *track, int side, uint32_t i)
{
    return track[(i / HALF) * BLOCK + side * HALF + (i % HALF)];
}

/*
 * Walk one side of a track: emit cells (out may be NULL: count only),
 * collect opcode effects in trk. Returns HFE_OK or HFE_BAD_OPCODE /
 * HFE_TOO_LARGE (more cells than cap_cells).
 */
static hfe_result_t walk(const uint8_t *track, int side, uint32_t len, bool v3,
                         uint32_t default_ns_x16, uint8_t *out, uint32_t cap_cells,
                         hfe_track_t *trk, uint16_t *ops)
{
    uint32_t n = 0;
    uint32_t cur_ns = default_ns_x16;

    memset(trk, 0, sizeof(*trk));
    trk->index_cell = -1;
    trk->nseg = 1;
    trk->seg[0] = (hfe_segment_t) { .start = 0, .ns_x16 = default_ns_x16 };

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
                if (trk->index_cell >= 0) {
                    return HFE_BAD_OPCODE;      /* more than one INDEX per revolution */
                }
                trk->index_cell = (int32_t)n;
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
                    if (trk->seg[trk->nseg - 1].start == n) {
                        trk->seg[trk->nseg - 1].ns_x16 = ns;    /* no cells in between */
                    } else if (trk->nseg < HFE_MAX_SEGMENTS) {
                        trk->seg[trk->nseg++] = (hfe_segment_t) { .start = n, .ns_x16 = ns };
                    } else {
                        return HFE_BAD_OPCODE;  /* too many bitrate changes */
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
            } else if (trk->nweak < HFE_MAX_WEAK) {
                trk->weak[trk->nweak++] = (hfe_weak_t) { .start = n, .count = 8 };
            } else {
                return HFE_BAD_OPCODE;
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
        }
        i++;
    }
    trk->cells = n;
    return HFE_OK;
}

/* Header and table checks shared by hfe_check and hfe_decode_track. */
static hfe_result_t header(const uint8_t *data, uint32_t size, hfe_info_t *info)
{
    memset(info, 0, sizeof(*info));
    if (size < HDR_SIZE) {
        snprintf(info->detail, sizeof(info->detail), "file too small for an HFE header");
        return HFE_NOT_HFE;
    }
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
    if ((v1 && revision != 0) || (v3 && revision != 0)) {
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

/* Track data of cylinder cyl: pointer and length of one side; bounds checked. */
static hfe_result_t track_ptr(const uint8_t *data, uint32_t size, const hfe_info_t *info,
                              int cyl, const uint8_t **track, uint32_t *len)
{
    const uint8_t *e = data + info->lut_block * BLOCK + cyl * 4;
    uint32_t off = (uint32_t)le16(e) * BLOCK;
    uint32_t both = le16(e + 2);
    *len = both / 2;
    uint32_t blocks = (*len + HALF - 1) / HALF;
    if (both == 0 || off < HDR_SIZE || off + blocks * BLOCK > size) {
        return HFE_BAD_TRACKS;
    }
    *track = data + off;
    return HFE_OK;
}

hfe_result_t hfe_check(const uint8_t *data, uint32_t size, uint32_t buffer_bytes,
                       hfe_info_t *info)
{
    hfe_result_t r = header(data, size, info);
    if (r != HFE_OK) {
        return r;
    }
    uint32_t ns = 500u * 16000u * 2 / info->bitrate / 2;   /* 1/16 ns per cell */
    static hfe_track_t trk;
    for (int c = 0; c < info->cylinders; c++) {
        const uint8_t *track;
        uint32_t len;
        if (track_ptr(data, size, info, c, &track, &len) != HFE_OK) {
            snprintf(info->detail, sizeof(info->detail), "track %d outside the file", c);
            return HFE_BAD_TRACKS;
        }
        for (int s = 0; s < info->sides; s++) {
            r = walk(track, s, len, info->version == 3, ns, NULL, HFE_MAX_TRACK_CELLS, &trk,
                     &info->opcodes);
            if (r != HFE_OK) {
                snprintf(info->detail, sizeof(info->detail), "track %d side %d: %s", c, s,
                         hfe_result_name(r));
                return r;
            }
            if (trk.cells < 1000) {
                snprintf(info->detail, sizeof(info->detail), "track %d side %d has only %lu cells",
                         c, s, (unsigned long)trk.cells);
                return HFE_BAD_TRACKS;
            }
            info->cells_total += trk.cells;
            info->bytes_needed += (trk.cells + 31) / 32 * 4;    /* 32-bit aligned */
            if (trk.cells > info->max_cells) {
                info->max_cells = trk.cells;
            }
            info->weak_areas += trk.nweak;
            info->bitrate_changes += trk.nseg - 1;
        }
    }
    if (info->bytes_needed > buffer_bytes) {
        snprintf(info->detail, sizeof(info->detail),
                 "its tracks need %lu KiB of track buffer, %lu KiB available",
                 (unsigned long)(info->bytes_needed / 1024), (unsigned long)(buffer_bytes / 1024));
        return HFE_TOO_LARGE;
    }
    snprintf(info->detail, sizeof(info->detail), "HFEv%u, %u cylinders, %u side(s), %u kbit/s%s",
             info->version, info->cylinders, info->sides, info->bitrate,
             info->opcodes ? ", with opcodes" : "");
    return HFE_OK;
}

hfe_result_t hfe_decode_track(const uint8_t *data, uint32_t size, const hfe_info_t *info,
                              int cyl, int side, uint8_t *out, uint32_t cap_bytes,
                              hfe_track_t *trk)
{
    const uint8_t *track;
    uint32_t len;
    uint16_t ops = 0;

    memset(trk, 0, sizeof(*trk));
    trk->index_cell = -1;
    if (cyl < 0 || cyl >= info->cylinders || side < 0 || side > 1) {
        return HFE_BAD_TRACKS;
    }
    if (side >= info->sides) {
        return HFE_OK;                          /* single-sided: no track */
    }
    if (track_ptr(data, size, info, cyl, &track, &len) != HFE_OK) {
        return HFE_BAD_TRACKS;
    }
    uint32_t ns = 500u * 16000u * 2 / info->bitrate / 2;
    hfe_result_t r = walk(track, side, len, info->version == 3, ns, out, cap_bytes * 8, trk, &ops);
    if (r != HFE_OK || trk->index_cell <= 0) {
        return r;
    }

    /* Rotate so the INDEX opcode position becomes cell 0. */
    uint32_t k = (uint32_t)trk->index_cell, n = trk->cells;
    /* In-place rotation of a bit array by reversal: rev(0,k), rev(k,n), rev(0,n). */
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
    /* Timing segments move with the cells: the one in force at old cell k
     * starts the track; later ones follow; earlier ones come after the wrap. */
    hfe_segment_t seg[HFE_MAX_SEGMENTS];
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
    for (int i = 0; i <= in_force && m < HFE_MAX_SEGMENTS; i++) {
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
