/*
 * Read-data flux stream and INDEX pulse via RMT. See flux_stream.h.
 *
 * Each flux transition becomes one RMT symbol: LOW until the transition,
 * then a FLUX_PULSE_TICKS HIGH pulse (GPIO HIGH = ULN2003A pulls /RDATA
 * low), so the leading edge of every /RDATA pulse is on the bitcell grid.
 * A track of N bitcells is spread over exactly FLUX_REV_TICKS (200 ms):
 * each cell lasts FLUX_REV_TICKS / N ticks, the remainder carried from
 * interval to interval (20 ticks = 2 us for the standard 100000 cells;
 * slightly less for the longer 11-sector tracks). A revolution therefore
 * always equals the INDEX loop period and both channels stay locked.
 *
 * The simple-encoder callback runs in the RMT/DMA ISR. It reads the raw
 * track (PSRAM) of the current cylinder and the live SIDE input for each
 * chunk, so a STEP or SIDE change reaches the pin within about one DMA
 * half buffer (FLUX_DMA_SYMBOLS / 2 transitions, ~1.4 ms).
 *
 * HFE mode (native timing, see flux_stream.h): the data encoder plays the
 * packed track (hfe.h) with its own cell times and records the RMT tick of
 * every revolution boundary; the INDEX channel is then an encoder too,
 * started in sync, that puts the pulse on exactly those ticks. It may
 * only run ahead of the data encoder in FLUX_INDEX_STEP pieces: its whole
 * memory (INDEX_MEM_SYMBOLS) is then at most 0.48 ms, less than the data
 * encoder always has queued (half its DMA buffer, >= 1 ms of flux), so a
 * boundary is known before INDEX gets there. Pulses that came late anyway
 * are counted (flux_hfe_stats).
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "esp_attr.h"

#include "board_pins.h"
#include "disk_image.h"
#include "drive_emu.h"
#include "flux_stream.h"
#include "hfe.h"

#define FLUX_DMA_SYMBOLS    512     /* DMA ping-pong buffer, internal RAM */
#define INDEX_MEM_SYMBOLS   48      /* one RMT memory block, no DMA */
#define RMT_MAX_DURATION    32767

/* Pin parked as plain GPIO driving its (LOW) output latch. */
#define GPIO_PARKED_SEL     (SIG_GPIO_OUT_IDX | GPIO_FUNC0_OEN_SEL_M)

static uint32_t rdata_rmt_sel;      /* GPIO matrix value with RMT connected */
static uint32_t index_rmt_sel;

static uint32_t cell_pos;           /* rotational position, 0..cells-1 */
static uint32_t cells_since_flux;
static uint32_t tick_frac;          /* timing remainder, in 1/cells ticks */

_Static_assert(DRIVE_MAX_TRACK < DISK_MAX_CYLS, "head position within the track buffers");
static volatile uint32_t revolutions;

static rmt_symbol_word_t index_symbols[INDEX_MEM_SYMBOLS];
static const uint8_t dummy_payload = 0;

static rmt_channel_handle_t data_chan, index_chan;
static rmt_encoder_handle_t data_enc, index_enc;          /* ST: MFM stream, INDEX loop */
static rmt_encoder_handle_t hfe_data_enc, hfe_index_enc;  /* HFE: native timing */
static flux_mode_t mode = FLUX_MODE_ST;

void IRAM_ATTR flux_gate(bool rdata, bool index)
{
    REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * PIN_FDD_RDATA,
              rdata ? rdata_rmt_sel : GPIO_PARKED_SEL);
    REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * PIN_FDD_INDEX,
              index ? index_rmt_sel : GPIO_PARKED_SEL);
}

uint32_t flux_revolutions(void)
{
    return revolutions;
}

/* Simple-encoder callback: endless flux stream (never sets *done). */
static size_t IRAM_ATTR flux_encode(const void *data, size_t data_size,
                                    size_t symbols_written, size_t symbols_free,
                                    rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    const uint8_t *raw = disk_track_raw(drive_cylinder(),
                                        gpio_ll_get_level(&GPIO, PIN_FDD_SIDE) ? 0 : 1);
    const uint32_t cells = disk_track_cells;
    uint32_t pos = cell_pos < cells ? cell_pos : 0;     /* track length may change */
    uint32_t dist = cells_since_flux;
    uint32_t frac = tick_frac % cells;
    size_t n = 0;

    while (n < symbols_free) {
        if (++pos == cells) {
            pos = 0;
            revolutions++;
        }
        dist++;
        /* dist >= 2: never two transitions 2 us apart, even right after a
         * track switch joins two different bitstreams. */
        if ((raw[pos >> 3] & (0x80 >> (pos & 7))) && dist >= 2) {
            if (dist > 1000) {
                dist = 1000;    /* no overflow; within the 15-bit RMT duration */
            }
            uint32_t num = frac + dist * FLUX_REV_TICKS;
            uint32_t ticks = num / cells;
            frac = num % cells;
            symbols[n++] = (rmt_symbol_word_t) {
                .level0 = 0, .duration0 = ticks - FLUX_PULSE_TICKS,
                .level1 = 1, .duration1 = FLUX_PULSE_TICKS,
            };
            dist = 0;
        }
    }

    tick_frac = frac;
    cell_pos = pos;
    cells_since_flux = dist;
    return n;
}

/* INDEX: HIGH (= /INDEX low) for INDEX_PULSE_MS, then LOW, 200 ms total. */
static size_t build_index_symbols(void)
{
    uint32_t high = INDEX_PULSE_MS * (FLUX_RESOLUTION_HZ / 1000);
    uint32_t total = FLUX_REV_TICKS;
    uint32_t parts[2 * INDEX_MEM_SYMBOLS];
    uint8_t levels[2 * INDEX_MEM_SYMBOLS];
    size_t np = 0;

    for (int level = 1; level >= 0; level--) {
        uint32_t left = level ? high : total - high;
        while (left) {
            uint32_t d = left > RMT_MAX_DURATION ? RMT_MAX_DURATION : left;
            parts[np] = d;
            levels[np++] = level;
            left -= d;
        }
    }
    if (np & 1) {
        /* Split the last LOW part so the parts pair up into symbols. */
        parts[np] = parts[np - 1] / 2;
        parts[np - 1] -= parts[np];
        levels[np] = 0;
        np++;
    }

    for (size_t i = 0; i < np / 2; i++) {
        index_symbols[i] = (rmt_symbol_word_t) {
            .level0 = levels[2 * i],     .duration0 = parts[2 * i],
            .level1 = levels[2 * i + 1], .duration1 = parts[2 * i + 1],
        };
    }
    return np / 2;
}

/* ---- HFE: native timing ------------------------------------------------------ */

#define TICK_X16        1600u       /* ns_x16 per RMT tick (0.1 us) */
#define FLUX_INDEX_STEP 100u        /* 10 us: INDEX filler while no boundary is known */
#define IDLE_TICKS      200u        /* 20 us: symbols without flux (no track, no disk) */
#define LONG_TICKS      30000u      /* a flux gap longer than this: LOW-only symbols */
#define NO_TRACK_REV    FLUX_REV_TICKS  /* rotation of an unformatted track: 200 ms */
#define BOUNDARY_RING   8

/* Data encoder state: RMT ticks since the sync start (wrapping, compared as differences). */
static uint32_t h_t;                /* end of the symbols generated so far */
static uint32_t h_rev_start;        /* tick of the last revolution boundary */
static const uint8_t *h_buf;        /* track buffer / cylinder / side being played */
static int h_cyl = -1, h_side = -1;
static hfe_packed_track_t h_trk;
static uint32_t h_pos;              /* cell of the track */
static uint8_t h_seg;               /* timing segment of h_pos */
static uint32_t h_gap;              /* time since the last flux, ns_x16 */
static uint32_t h_frac;             /* tick remainder, ns_x16 (< TICK_X16) */
static uint32_t h_cells_since;      /* cells since the last flux */

/* Boundaries: data encoder -> INDEX encoder (both in the RMT ISR). */
static volatile uint32_t ring[BOUNDARY_RING];
static volatile uint8_t ring_head, ring_tail;

/* INDEX encoder state */
static uint32_t i_t;
static uint32_t i_high;             /* ticks of the current pulse still to send */
static volatile uint32_t late_pulses, late_max_ticks, lost_boundaries;

static void IRAM_ATTR boundary_at(uint32_t tick)
{
    uint8_t next = (ring_head + 1) % BOUNDARY_RING;
    if (next == ring_tail) {
        lost_boundaries++;
        return;
    }
    ring[ring_head] = tick;
    ring_head = next;
    h_rev_start = tick;
    revolutions++;
}

/* The packed track (see hfe_packed_track), read in the ISR. */
static void IRAM_ATTR packed_track(const uint8_t *buf, int cyl, int side, hfe_packed_track_t *t)
{
    const hfe_slot_t *s = (const hfe_slot_t *)buf + cyl * 2 + side;
    t->cells = NULL;
    t->count = 0;
    if (cyl < HFE_MAX_CYLS && s->offset && s->cells && s->nseg) {
        t->cells = buf + s->offset;
        t->count = s->cells;
        t->seg = (const hfe_segment_t *)(buf + s->offset + (s->cells + 31) / 32 * 4);
        t->nseg = s->nseg;
        t->weak = (const hfe_weak_t *)(t->seg + s->nseg);
        t->nweak = s->nweak;
    }
}

/* Another track (STEP, SIDE, disk change): continue at the same time
 * since the last boundary; past its end, the boundary is now. */
static void IRAM_ATTR hfe_enter_track(const uint8_t *buf, int cyl, int side)
{
    h_buf = buf;
    h_cyl = cyl;
    h_side = side;
    packed_track(buf, cyl, side, &h_trk);
    h_pos = 0;
    h_seg = 0;
    if (!h_trk.cells) {
        return;
    }
    /* Elapsed time since the boundary (up to now, not just the symbols
     * generated), then walk the segments. */
    uint32_t now = h_t + (h_frac + h_gap) / TICK_X16;
    uint64_t left = (uint64_t)(now - h_rev_start) * TICK_X16;
    for (int i = 0; i < h_trk.nseg; i++) {
        uint32_t end = i + 1 < h_trk.nseg ? h_trk.seg[i + 1].start : h_trk.count;
        uint64_t span = (uint64_t)(end - h_trk.seg[i].start) * h_trk.seg[i].ns_x16;
        if (left < span) {
            h_pos = h_trk.seg[i].start + (uint32_t)(left / h_trk.seg[i].ns_x16);
            h_seg = (uint8_t)i;
            return;
        }
        left -= span;
    }
    boundary_at(now);               /* this track is shorter: its revolution is over */
}

static inline rmt_symbol_word_t IRAM_ATTR low_symbol(uint32_t ticks)
{
    return (rmt_symbol_word_t) { .level0 = 0, .duration0 = ticks / 2,
                                 .level1 = 0, .duration1 = ticks - ticks / 2 };
}

/* HFE data encoder: endless flux stream at the track's own cell times. */
static size_t IRAM_ATTR hfe_encode(const void *data, size_t data_size,
                                   size_t symbols_written, size_t symbols_free,
                                   rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    size_t n = 0;
    const uint8_t *buf = (const uint8_t *)disk_tracks;

    if (disk_tracks_kind != DISK_TRACKS_HFE) {
        /* Right after a disk change, before the mode follows: no flux. */
        while (n < symbols_free) {
            symbols[n++] = low_symbol(IDLE_TICKS);
            h_t += IDLE_TICKS;
            if (h_t - h_rev_start >= NO_TRACK_REV) {
                boundary_at(h_t);
            }
        }
        h_buf = NULL;
        return n;
    }
    int cyl = drive_cylinder();
    int side = gpio_ll_get_level(&GPIO, PIN_FDD_SIDE) ? 0 : 1;
    if (buf != h_buf || cyl != h_cyl || side != h_side) {
        hfe_enter_track(buf, cyl, side);
    }

    while (n < symbols_free) {
        if (!h_trk.cells) {
            /* No track here: no flux, the disk still turns. */
            uint32_t d = IDLE_TICKS;
            symbols[n++] = low_symbol(d);
            h_t += d;
            if (h_t - h_rev_start >= NO_TRACK_REV) {
                boundary_at(h_t);
            }
            continue;
        }
        if (++h_pos == h_trk.count) {
            /* Revolution boundary at the start of cell 0. */
            h_pos = 0;
            h_seg = 0;
            boundary_at(h_t + (h_frac + h_gap) / TICK_X16);
        } else if (h_seg + 1 < h_trk.nseg && h_pos == h_trk.seg[h_seg + 1].start) {
            h_seg++;
        }
        h_gap += h_trk.seg[h_seg].ns_x16;
        h_cells_since++;
        /* Two cells at least between transitions, also across a track change. */
        if ((h_trk.cells[h_pos >> 3] & (0x80 >> (h_pos & 7))) && h_cells_since >= 2) {
            uint32_t num = h_frac + h_gap;
            uint32_t ticks = num / TICK_X16;
            h_frac = num % TICK_X16;
            symbols[n++] = (rmt_symbol_word_t) {
                .level0 = 0, .duration0 = ticks - FLUX_PULSE_TICKS,
                .level1 = 1, .duration1 = FLUX_PULSE_TICKS,
            };
            h_t += ticks;
            h_gap = 0;
            h_cells_since = 0;
        } else if (h_gap >= LONG_TICKS * TICK_X16) {
            /* A long stretch without flux: pass the time on without a pulse. */
            symbols[n++] = low_symbol(LONG_TICKS);
            h_t += LONG_TICKS;
            h_gap -= LONG_TICKS * TICK_X16;
        }
    }
    return n;
}

/* HFE INDEX encoder: the pulse on the boundary ticks the data encoder recorded. */
static size_t IRAM_ATTR hfe_index_encode(const void *data, size_t data_size,
                                         size_t symbols_written, size_t symbols_free,
                                         rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    const uint32_t pulse = INDEX_PULSE_MS * (FLUX_RESOLUTION_HZ / 1000);
    size_t n = 0;

    while (n < symbols_free) {
        if (i_high) {
            symbols[n++] = (rmt_symbol_word_t) { .level0 = 1, .duration0 = i_high / 2,
                                                 .level1 = 1, .duration1 = i_high - i_high / 2 };
            i_t += i_high;
            i_high = 0;
            continue;
        }
        if (ring_tail != ring_head) {
            int32_t gap = (int32_t)(ring[ring_tail] - i_t);
            if (gap < 2) {
                if (gap < 0) {
                    late_pulses++;
                    if ((uint32_t)-gap > late_max_ticks) {
                        late_max_ticks = (uint32_t)-gap;
                    }
                }
                ring_tail = (ring_tail + 1) % BOUNDARY_RING;
                i_high = pulse;
                continue;
            }
            uint32_t d = gap > 2 * RMT_MAX_DURATION ? 2 * RMT_MAX_DURATION : (uint32_t)gap;
            symbols[n++] = low_symbol(d);
            i_t += d;
            continue;
        }
        symbols[n++] = low_symbol(FLUX_INDEX_STEP);     /* boundary not known yet */
        i_t += FLUX_INDEX_STEP;
    }
    return n;
}

void flux_hfe_stats(flux_hfe_stats_t *st)
{
    st->late_pulses = late_pulses;
    st->late_max_us = late_max_ticks / (FLUX_RESOLUTION_HZ / 1000000);
    st->lost_boundaries = lost_boundaries;
}

/* ---- Start / restart ------------------------------------------------------------- */

/* Start both channels at the same moment, in the current mode. */
static esp_err_t start_channels(void)
{
    rmt_channel_handle_t chans[] = { data_chan, index_chan };
    const rmt_sync_manager_config_t sync_cfg = {
        .tx_channel_array = chans,
        .array_size = 2,
    };
    rmt_sync_manager_handle_t sync;
    ESP_ERROR_CHECK(rmt_new_sync_manager(&sync_cfg, &sync));

    if (mode == FLUX_MODE_ST) {
        size_t n_index = build_index_symbols();
        const rmt_transmit_config_t index_tx = { .loop_count = -1 };
        ESP_ERROR_CHECK(rmt_transmit(index_chan, index_enc, index_symbols,
                                     n_index * sizeof(rmt_symbol_word_t), &index_tx));
    } else {
        const rmt_transmit_config_t index_tx = { .loop_count = 0 };
        ESP_ERROR_CHECK(rmt_transmit(index_chan, hfe_index_enc, &dummy_payload,
                                     sizeof(dummy_payload), &index_tx));
    }
    const rmt_transmit_config_t data_tx = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(data_chan, mode == FLUX_MODE_ST ? data_enc : hfe_data_enc,
                                 &dummy_payload, sizeof(dummy_payload), &data_tx));

    /* Both channels now run forever. The hardware start synchronisation
     * (group-wide tx_sim_en) is no longer needed and must not affect the
     * other RMT channels (status LED), so remove it again. */
    ESP_ERROR_CHECK(rmt_del_sync_manager(sync));
    return ESP_OK;
}

esp_err_t flux_stream_init(void)
{
    const rmt_tx_channel_config_t data_cfg = {
        .gpio_num = PIN_FDD_RDATA,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = FLUX_RESOLUTION_HZ,
        .mem_block_symbols = FLUX_DMA_SYMBOLS,
        .trans_queue_depth = 1,
        .flags.with_dma = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&data_cfg, &data_chan);
    if (err != ESP_OK) {
        printf("ERROR: RMT data channel: %s\n", esp_err_to_name(err));
        return err;
    }
    rdata_rmt_sel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * PIN_FDD_RDATA);

    const rmt_tx_channel_config_t index_cfg = {
        .gpio_num = PIN_FDD_INDEX,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = FLUX_RESOLUTION_HZ,
        .mem_block_symbols = INDEX_MEM_SYMBOLS,
        .trans_queue_depth = 1,
    };
    err = rmt_new_tx_channel(&index_cfg, &index_chan);
    if (err != ESP_OK) {
        printf("ERROR: RMT index channel: %s\n", esp_err_to_name(err));
        return err;
    }
    index_rmt_sel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * PIN_FDD_INDEX);

    /* Park both pins right away: LOW latch, output enabled, GPIO driven. */
    gpio_set_level(PIN_FDD_RDATA, 0);
    gpio_set_level(PIN_FDD_INDEX, 0);
    gpio_ll_output_enable(&GPIO, PIN_FDD_RDATA);
    gpio_ll_output_enable(&GPIO, PIN_FDD_INDEX);
    flux_gate(false, false);

    const rmt_simple_encoder_config_t data_enc_cfg = {
        .callback = flux_encode,
        .min_chunk_size = 1,
    };
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&data_enc_cfg, &data_enc));
    const rmt_copy_encoder_config_t index_enc_cfg = { };
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&index_enc_cfg, &index_enc));
    const rmt_simple_encoder_config_t hfe_data_cfg = {
        .callback = hfe_encode,
        .min_chunk_size = 1,
    };
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&hfe_data_cfg, &hfe_data_enc));
    const rmt_simple_encoder_config_t hfe_index_cfg = {
        .callback = hfe_index_encode,
        .min_chunk_size = 1,
    };
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&hfe_index_cfg, &hfe_index_enc));

    ESP_ERROR_CHECK(rmt_enable(data_chan));
    ESP_ERROR_CHECK(rmt_enable(index_chan));

    /* Start both channels at the same moment: INDEX at the track start. */
    return start_channels();
}

esp_err_t flux_stream_set_mode(flux_mode_t m)
{
    if (!data_chan) {
        mode = m;                       /* not started (yet): flux_stream_init uses it */
        return ESP_OK;
    }
    if (m == mode) {
        return ESP_OK;
    }
    /* Stop both, reset every encoder state, start again together at tick 0. */
    ESP_ERROR_CHECK(rmt_disable(data_chan));
    ESP_ERROR_CHECK(rmt_disable(index_chan));
    rmt_encoder_reset(data_enc);
    rmt_encoder_reset(index_enc);
    rmt_encoder_reset(hfe_data_enc);
    rmt_encoder_reset(hfe_index_enc);
    cell_pos = 0;
    cells_since_flux = 0;
    tick_frac = 0;
    h_t = h_rev_start = 0;
    h_buf = NULL;
    h_cyl = h_side = -1;
    h_gap = h_frac = h_cells_since = 0;
    ring[0] = 0;                    /* INDEX right at the start, as in ST mode */
    ring_head = 1;
    ring_tail = 0;
    i_t = 0;
    i_high = 0;
    mode = m;
    ESP_ERROR_CHECK(rmt_enable(data_chan));
    ESP_ERROR_CHECK(rmt_enable(index_chan));
    printf("Flux: %s mode\n", m == FLUX_MODE_ST ? "ST (200 ms rotation)" : "HFE (native timing)");
    return start_channels();
}

flux_mode_t flux_stream_mode(void)
{
    return mode;
}
