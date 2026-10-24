/*
 * Flux monitor (debug tool): measures what the emulated drive really puts
 * on its INDEX and RDATA pins, while the computer reads the disk.
 *
 * - INDEX: MCPWM capture (12.5 ns) of every pulse edge: the period of each
 *   revolution and the pulse width, per track, against the duration the
 *   track should have (HFE: the sum of its cell times; ST: 200 ms).
 * - RDATA: after an INDEX pulse an RMT receiver records MON_SYMBOLS flux
 *   pulses; their intervals are compared with the track in the track
 *   buffer (the same cells and cell times the encoder plays). A missing,
 *   extra or misplaced pulse shows as a mismatch, with where it is.
 *
 * Only while the drive is selected: the pins are gated otherwise. Off by
 * default; switched on through the API (GET/PUT /api/v1/debug/flux).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define MON_FAILURES    16      /* the last content checks that failed */

typedef struct {
    uint32_t revs;              /* revolutions measured on this track */
    uint32_t period_min_ns, period_max_ns;
    uint32_t expected_ns;       /* what a revolution of this track should last */
    uint32_t pulse_min_ns, pulse_max_ns;    /* INDEX pulse width */
    uint32_t checks;            /* RDATA content checks */
    uint32_t clean;             /* ... without any mismatch */
    uint32_t unaligned;         /* ... where the captured pulses were not found in the track */
    uint32_t intervals;         /* flux intervals compared */
    uint32_t bad;               /* ... that did not match */
    uint32_t align_min_ns, align_max_ns;    /* INDEX to the first compared pulse */
} mon_track_t;

typedef struct {
    uint8_t cyl, side;
    uint32_t at;                /* interval number of the first mismatch */
    uint32_t expected_ns, got_ns;
    uint32_t cell;              /* cell of the track where it is */
    uint32_t bad, intervals;    /* in this check */
} mon_failure_t;

/* Start (first call creates the capture channels) or stop measuring. */
esp_err_t flux_monitor_enable(bool on);
bool flux_monitor_enabled(void);
void flux_monitor_reset(void);

/* Results of track cyl/side (false: nothing measured there). */
bool flux_monitor_track(int cyl, int side, mon_track_t *out);
/* The last failures, newest first; returns how many. */
int flux_monitor_failures(mon_failure_t *out, int max);
