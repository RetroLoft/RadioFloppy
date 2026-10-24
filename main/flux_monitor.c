/*
 * Flux monitor (debug tool). See flux_monitor.h.
 *
 * The capture ISR (INDEX edges) only timestamps and hands the edge to the
 * monitor task (core 1, low priority); the task does the bookkeeping,
 * starts an RDATA capture right after an INDEX pulse and compares it with
 * the track. The RDATA capture starts some tens of microseconds after the
 * pulse (task latency), so the captured pulses are first located in the
 * track (16 intervals in a row that match) and then compared one by one.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/mcpwm_cap.h"
#include "driver/rmt_rx.h"
#include "esp_heap_caps.h"
#include "hal/gpio_ll.h"

#include "board_pins.h"
#include "disk_image.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "flux_monitor.h"
#include "flux_stream.h"
#include "hfe.h"

#define RX_HZ           20000000    /* 50 ns per RMT tick */
#define RX_NS           (1000000000u / RX_HZ)
#define MON_SYMBOLS     4096        /* flux pulses per content check */
#define MON_MATCH_RUN   16          /* intervals in a row that locate the capture */
#define MON_SEARCH_NS   20000000u   /* look for it in the first 20 ms after INDEX */
#define MON_TOL_NS      500         /* an interval matches within +-0.5 us */
#define MON_MAX_FLUX    49152       /* flux transitions of one track (MFM: about 40000) */

typedef struct {
    uint32_t rise;                  /* capture ticks: pulse start */
    uint32_t width;                 /* pulse length */
    uint32_t head;                  /* drive_head_events() at the pulse end */
    uint8_t cyl, side;
} index_ev_t;

#define PULSE_TOL_NS    20000       /* a whole INDEX pulse: 3 ms +- 20 us (else cut by the gate) */

static volatile bool enabled;
static bool created;
static mcpwm_cap_timer_handle_t cap_timer;
static mcpwm_cap_channel_handle_t cap_index;
static uint32_t cap_hz;
static QueueHandle_t ev_queue, rx_queue;
static rmt_channel_handle_t rx_chan;
static rmt_symbol_word_t *rx_buf;          /* the driver fills it ... */
static rmt_symbol_word_t *rx_copy;         /* ... and starts again: the first fill kept here */
static volatile bool rx_taken;

static portMUX_TYPE res_lock = portMUX_INITIALIZER_UNLOCKED;
static mon_track_t tracks[DISK_MAX_CYLS][2];
static mon_failure_t failures[MON_FAILURES];
static int fail_head, fail_count;

/* Expected flux of one track: interval before each transition (ns, at
 * most 65.535 us) and its cell. 288 KiB of PSRAM, only once the monitor
 * is switched on. */
static uint16_t *exp_ns;
static uint32_t *exp_cell;
static uint32_t exp_n;
static const void *exp_buf;
static int exp_cyl = -1, exp_side = -1;

/* ---- INDEX capture ---------------------------------------------------------------- */

static bool IRAM_ATTR on_index_edge(mcpwm_cap_channel_handle_t ch, const mcpwm_capture_event_data_t *e,
                                    void *arg)
{
    static uint32_t last_rise;
    BaseType_t woken = pdFALSE;

    /* Pin HIGH = /INDEX pulled low. The event goes out at the pulse end,
     * with its start and length (a pulse cut by the gate is shorter). */
    if (e->cap_edge == MCPWM_CAP_EDGE_POS) {
        last_rise = e->cap_value;
    } else if (enabled) {
        index_ev_t ev = {
            .rise = last_rise,
            .width = e->cap_value - last_rise,
            .head = drive_head_events(),
            .cyl = (uint8_t)drive_cylinder(),
            .side = gpio_ll_get_level(&GPIO, PIN_FDD_SIDE) ? 0 : 1,
        };
        xQueueSendFromISR(ev_queue, &ev, &woken);
    }
    return woken == pdTRUE;
}

/* With partial receive the driver reuses the buffer right after this
 * callback: keep the first fill. */
static bool on_rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    if (rx_taken) {
        return false;
    }
    rx_taken = true;
    size_t n = e->num_symbols < MON_SYMBOLS ? e->num_symbols : MON_SYMBOLS;
    memcpy(rx_copy, e->received_symbols, n * sizeof(rmt_symbol_word_t));
    xQueueSendFromISR(rx_queue, &n, &woken);
    return woken == pdTRUE;
}

/* ---- Expected flux --------------------------------------------------------------------- */

/* Build the transitions of track cyl/side as the encoder plays them
 * (flux_stream.c: at least two cells between transitions). */
static bool build_expected(int cyl, int side)
{
    const uint8_t *buf = (const uint8_t *)disk_tracks;
    if (buf == exp_buf && cyl == exp_cyl && side == exp_side) {
        return exp_n > 0;
    }
    exp_buf = buf;
    exp_cyl = cyl;
    exp_side = side;
    exp_n = 0;

    const uint8_t *cells;
    uint32_t count;
    hfe_packed_track_t t = { 0 };
    uint64_t st_x16 = 0;                    /* ST: one cell time, ns x16 */
    if (disk_tracks_kind == DISK_TRACKS_HFE) {
        hfe_packed_track(buf, cyl, side, &t);
        if (!t.cells) {
            return false;
        }
        cells = t.cells;
        count = t.count;
    } else {
        cells = disk_track_raw(cyl, side);
        count = disk_track_cells;
        st_x16 = (uint64_t)FLUX_REV_TICKS * 1600u / count;  /* 100 ns ticks -> ns x16 */
    }
    /* Two passes: the first finds the time after the last transition, which
     * belongs to the first interval of the next revolution. */
    uint64_t tail = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t gap = pass ? tail : 0;
        uint32_t since = 2, seg = 0;
        for (uint32_t p = 0; p < count; p++) {
            if (t.cells && seg + 1 < t.nseg && p == t.seg[seg + 1].start) {
                seg++;
            }
            uint32_t ns_x16 = t.cells ? t.seg[seg].ns_x16 : (uint32_t)st_x16;
            gap += ns_x16;
            since++;
            if ((cells[p >> 3] & (0x80 >> (p & 7))) && since >= 2) {
                if (pass && exp_n < MON_MAX_FLUX) {
                    exp_ns[exp_n] = gap / 16 > UINT16_MAX ? UINT16_MAX : (uint16_t)(gap / 16);
                    exp_cell[exp_n] = p;
                    exp_n++;
                }
                gap = 0;
                since = 0;
            }
        }
        tail = gap;
    }
    return exp_n > MON_MATCH_RUN;
}

static uint32_t track_duration_ns(int cyl, int side)
{
    if (disk_tracks_kind != DISK_TRACKS_HFE) {
        return FLUX_REV_TICKS * 100u;
    }
    hfe_packed_track_t t;
    hfe_packed_track((const uint8_t *)disk_tracks, cyl, side, &t);
    if (!t.cells) {
        return FLUX_REV_TICKS * 100u;       /* no track: the disk turns at 300 rpm */
    }
    uint64_t sum = 0;
    for (int i = 0; i < t.nseg; i++) {
        uint32_t end = i + 1 < t.nseg ? t.seg[i + 1].start : t.count;
        sum += (uint64_t)(end - t.seg[i].start) * t.seg[i].ns_x16;
    }
    return (uint32_t)(sum / 16);
}

static inline bool near(uint32_t a, uint32_t b)
{
    return a > b ? a - b <= MON_TOL_NS : b - a <= MON_TOL_NS;
}

/* ---- Content check ------------------------------------------------------------------------ */

/* Intervals between the rising edges (pulse starts) of the captured symbols. */
static uint32_t to_intervals(const rmt_symbol_word_t *s, size_t n, uint32_t *out, uint32_t max)
{
    uint32_t t = 0, m = 0;
    int64_t last = -1;
    int prev = 1;                           /* skip a pulse already running at the start */

    for (size_t i = 0; i < n && m < max; i++) {
        unsigned lv[2] = { s[i].level0, s[i].level1 };
        unsigned du[2] = { s[i].duration0, s[i].duration1 };
        for (int k = 0; k < 2; k++) {
            if (lv[k] == 1 && prev == 0) {
                if (last >= 0 && m < max) {
                    out[m++] = (t - (uint32_t)last) * RX_NS;
                }
                last = t;
            }
            prev = lv[k];
            if (du[k] == 0) {
                return m;
            }
            t += du[k];
        }
    }
    return m;
}

static void content_check(int cyl, int side, const uint32_t *iv, uint32_t m)
{
    if (!build_expected(cyl, side) || m < MON_MATCH_RUN * 2) {
        return;
    }
    /* Locate: the transition k from which the most intervals in a row
     * match (at least MON_MATCH_RUN). Not simply the first: a gap of
     * repeated bytes matches at many places. */
    int64_t k0 = -1;
    uint32_t best = MON_MATCH_RUN - 1;
    uint32_t at = 0, align = 0;             /* about the time from INDEX to transition k */
    for (uint32_t k = 0; k < exp_n && at < MON_SEARCH_NS && best < m; at += exp_ns[++k % exp_n]) {
        uint32_t j = 0;
        while (j < m && near(iv[j], exp_ns[(k + j) % exp_n])) {
            j++;
        }
        if (j > best) {
            best = j;
            k0 = k;
            align = at;
        }
    }
    mon_failure_t f = { .cyl = (uint8_t)cyl, .side = (uint8_t)side, .intervals = m };
    bool first = true;
    if (k0 >= 0) {
        for (uint32_t i = 0; i < m; i++) {
            uint32_t e = (uint32_t)((k0 + i) % exp_n);
            if (!near(iv[i], exp_ns[e])) {
                if (first) {
                    f.at = i;
                    f.expected_ns = exp_ns[e];
                    f.got_ns = iv[i];
                    f.cell = exp_cell[e];
                    first = false;
                }
                f.bad++;
            }
        }
    }
    portENTER_CRITICAL(&res_lock);
    mon_track_t *t = &tracks[cyl][side];
    t->checks++;
    if (k0 < 0) {
        t->unaligned++;
    } else {
        if (t->checks - t->unaligned == 1 || align < t->align_min_ns) {
            t->align_min_ns = align;
        }
        if (align > t->align_max_ns) {
            t->align_max_ns = align;
        }
        t->intervals += m;
        t->bad += f.bad;
        if (!f.bad) {
            t->clean++;
        }
    }
    if (k0 >= 0 && f.bad) {
        failures[fail_head] = f;
        fail_head = (fail_head + 1) % MON_FAILURES;
        if (fail_count < MON_FAILURES) {
            fail_count++;
        }
    }
    portEXIT_CRITICAL(&res_lock);
    if (k0 < 0) {
        printf("Flux monitor: C%d H%d - captured pulses not found in the track\n", cyl, side);
    } else if (f.bad) {
        printf("Flux monitor: C%d H%d - %lu of %lu intervals differ, first at #%lu (cell %lu): "
               "%lu ns instead of %lu ns\n", cyl, side, (unsigned long)f.bad, (unsigned long)m,
               (unsigned long)f.at, (unsigned long)f.cell, (unsigned long)f.got_ns,
               (unsigned long)f.expected_ns);
    }
}

/* ---- Task ---------------------------------------------------------------------------------- */

static void monitor_task(void *arg)
{
    static uint32_t iv[MON_SYMBOLS];
    const rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = 100,
        .signal_range_max_ns = 1600000,     /* 1.6 ms without a pulse: the capture ends */
        .flags.en_partial_rx = 1,           /* more than the RMT memory: piece by piece */
    };
    index_ev_t ev, prev = { .cyl = 0xff };
    const uint32_t pulse_ns = INDEX_PULSE_MS * 1000000u;

    while (true) {
        xQueueReceive(ev_queue, &ev, portMAX_DELAY);
        if (!enabled) {
            continue;
        }
        uint32_t width = (uint32_t)((uint64_t)ev.width * 1000000000u / cap_hz);
        uint32_t prev_width = (uint32_t)((uint64_t)prev.width * 1000000000u / cap_hz);
        bool whole = width > pulse_ns - PULSE_TOL_NS && width < pulse_ns + PULSE_TOL_NS &&
                     prev_width > pulse_ns - PULSE_TOL_NS && prev_width < pulse_ns + PULSE_TOL_NS;
        bool stable = whole && ev.head == prev.head && ev.cyl < DISK_MAX_CYLS;
        uint32_t period = (uint32_t)((uint64_t)(ev.rise - prev.rise) * 1000000000u / cap_hz);
        prev = ev;
        if (stable && period < 2 * FLUX_REV_TICKS * 100u) {
            uint32_t expected = track_duration_ns(ev.cyl, ev.side);
            portENTER_CRITICAL(&res_lock);
            mon_track_t *t = &tracks[ev.cyl][ev.side];
            if (!t->revs || period < t->period_min_ns) t->period_min_ns = period;
            if (period > t->period_max_ns) t->period_max_ns = period;
            if (!t->revs || width < t->pulse_min_ns) t->pulse_min_ns = width;
            if (width > t->pulse_max_ns) t->pulse_max_ns = width;
            t->expected_ns = expected;
            t->revs++;
            portEXIT_CRITICAL(&res_lock);
        }
        /* Right after this pulse: record the flux and compare it. */
        size_t got = 0;
        xQueueReset(rx_queue);
        rx_taken = false;
        if (rmt_receive(rx_chan, rx_buf, MON_SYMBOLS * sizeof(rmt_symbol_word_t), &rx_cfg) != ESP_OK) {
            continue;
        }
        bool done = xQueueReceive(rx_queue, &got, pdMS_TO_TICKS(150)) == pdTRUE;
        rmt_disable(rx_chan);               /* ends a reception still running */
        rmt_enable(rx_chan);
        int cyl = ev.cyl, side = ev.side;
        if (!done || drive_head_events() != ev.head || !emulator_is_selected()) {
            xQueueReset(ev_queue);          /* the track changed meanwhile: skip */
            continue;
        }
        uint32_t m = to_intervals(rx_copy, got, iv, MON_SYMBOLS);
        content_check(cyl, side, iv, m);
        xQueueReset(ev_queue);              /* compare at most one revolution in two */
    }
}

/* ---- API ------------------------------------------------------------------------------------- */

static esp_err_t create(void)
{
    exp_ns = heap_caps_malloc(MON_MAX_FLUX * sizeof(*exp_ns), MALLOC_CAP_SPIRAM);
    exp_cell = heap_caps_malloc(MON_MAX_FLUX * sizeof(*exp_cell), MALLOC_CAP_SPIRAM);
    rx_buf = heap_caps_malloc(MON_SYMBOLS * sizeof(rmt_symbol_word_t),
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rx_copy = heap_caps_malloc(MON_SYMBOLS * sizeof(rmt_symbol_word_t), MALLOC_CAP_SPIRAM);
    ev_queue = xQueueCreate(8, sizeof(index_ev_t));
    rx_queue = xQueueCreate(4, sizeof(size_t));
    if (!exp_ns || !exp_cell || !rx_buf || !rx_copy || !ev_queue || !rx_queue) {
        return ESP_ERR_NO_MEM;
    }

    const mcpwm_capture_timer_config_t tcfg = { .group_id = 0, .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT };
    esp_err_t err = mcpwm_new_capture_timer(&tcfg, &cap_timer);
    if (err == ESP_OK) {
        mcpwm_capture_timer_get_resolution(cap_timer, &cap_hz);
        /* An input on the INDEX pin as well: its pad shows what the RMT drives. */
        const mcpwm_capture_channel_config_t ccfg = {
            .gpio_num = PIN_FDD_INDEX, .prescale = 1,
            .flags.pos_edge = true, .flags.neg_edge = true,
        };
        err = mcpwm_new_capture_channel(cap_timer, &ccfg, &cap_index);
    }
    if (err == ESP_OK) {
        const mcpwm_capture_event_callbacks_t cbs = { .on_cap = on_index_edge };
        err = mcpwm_capture_channel_register_event_callbacks(cap_index, &cbs, NULL);
    }
    if (err == ESP_OK) err = mcpwm_capture_channel_enable(cap_index);
    if (err == ESP_OK) err = mcpwm_capture_timer_enable(cap_timer);
    if (err == ESP_OK) err = mcpwm_capture_timer_start(cap_timer);
    if (err == ESP_OK) {
        const rmt_rx_channel_config_t rcfg = {
            .gpio_num = PIN_FDD_RDATA,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = RX_HZ,
            .mem_block_symbols = 96,        /* two RMT memory blocks, ping-pong */
        };
        err = rmt_new_rx_channel(&rcfg, &rx_chan);
    }
    if (err == ESP_OK) {
        const rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_rx_done };
        err = rmt_rx_register_event_callbacks(rx_chan, &cbs, NULL);
    }
    if (err == ESP_OK) err = rmt_enable(rx_chan);
    if (err == ESP_OK) {
        xTaskCreatePinnedToCore(monitor_task, "flux_mon", 4096, NULL, 2, NULL, 1);
        printf("Flux monitor: INDEX capture at %lu MHz, RDATA capture at %d MHz\n",
               (unsigned long)(cap_hz / 1000000), RX_HZ / 1000000);
    }
    return err;
}

esp_err_t flux_monitor_enable(bool on)
{
    if (on && !created) {
        esp_err_t err = create();       /* the caller's core (the HTTP server: core 1) */
        if (err != ESP_OK) {
            printf("Flux monitor: not available (%s)\n", esp_err_to_name(err));
            return err;
        }
        created = true;
    }
    enabled = on && created;
    printf("Flux monitor: %s\n", enabled ? "on" : "off");
    return ESP_OK;
}

bool flux_monitor_enabled(void)
{
    return enabled;
}

void flux_monitor_reset(void)
{
    portENTER_CRITICAL(&res_lock);
    memset(tracks, 0, sizeof(tracks));
    fail_head = fail_count = 0;
    portEXIT_CRITICAL(&res_lock);
}

bool flux_monitor_track(int cyl, int side, mon_track_t *out)
{
    portENTER_CRITICAL(&res_lock);
    *out = tracks[cyl][side];
    portEXIT_CRITICAL(&res_lock);
    return out->revs || out->checks;
}

int flux_monitor_failures(mon_failure_t *out, int max)
{
    portENTER_CRITICAL(&res_lock);
    int n = fail_count < max ? fail_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = failures[(fail_head - 1 - i + MON_FAILURES) % MON_FAILURES];
    }
    portEXIT_CRITICAL(&res_lock);
    return n;
}
