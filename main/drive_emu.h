/*
 * Virtual Shugart drive: selection, head position and outputs.
 *
 * All output decisions are made here, in ISR context, from the live input
 * levels, so they never wait for logging or a background task:
 *
 *   active  = armed && selected (EMU_SELECT_LINE LOW)
 *   disk    = a disk is inserted and no disk change is being signalled
 *   TRACK0  = active && cylinder == 0
 *   WPROT   = active && !writable           (read-only disk, see drive_set_writable)
 *   INDEX   = active && disk && MOTOR on
 *   RDATA   = active && disk && MOTOR on && WGATE inactive
 *   READY, DSKCHG: never driven
 *
 * Disk change: for DRIVE_MEDIA_CHANGE_MS after a swap WPROT is inverted
 * (released for a read-only disk, asserted for a writable one) and
 * INDEX/RDATA stay off, as with an open drive door. TOS notices a disk
 * change through that write-protect transition.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Highest virtual head position (a real drive reaches about 83). */
#define DRIVE_MAX_TRACK 83

typedef struct {
    bool armed;
    bool selected;      /* emulator_is_selected() at the time of the update */
    bool motor;
    bool wgate;
    bool track0;        /* outputs as driven after the update */
    bool wprot;
    bool rdata;
    bool index;
    int8_t cyl;
    int8_t side;
} drive_status_t;

typedef enum {
    DRV_EV_SELECT,
    DRV_EV_MOTOR,
    DRV_EV_WGATE,
    DRV_EV_SIDE,
    DRV_EV_STEP,
} drive_event_type_t;

typedef struct {
    int64_t time_us;
    drive_status_t st;  /* after the ISR update */
    uint8_t type;
} drive_event_t;

/* Start of a write (WGATE asserted while selected), taken in the ISR. */
typedef struct {
    int64_t time_us;        /* WGATE asserted */
    int64_t index_us;       /* start of the INDEX pulse before it */
    int8_t cyl;
    int8_t side;
    bool writable;          /* WPROT was released: the write is allowed */
    uint32_t gen;           /* disk_media_gen at that moment */
    uint32_t seq;           /* +1 per write */
} drive_write_start_t;

/* Configure the inputs and install the interrupts (drive not armed). */
void drive_init(void);

/* Release (true) or assert (false) write protection for the inserted disk.
 * Only disk_write.c decides this; default: protected. */
void drive_set_writable(bool on);
bool drive_writable(void);

void drive_get_write_start(drive_write_start_t *ws);

/* Arm the drive (once, after the start-up guard) and apply the outputs. */
void drive_arm(drive_status_t *status);

/* Current state without touching the outputs (for displays). */
void drive_peek(drive_status_t *status);

/* Re-evaluate the outputs from the live input levels (safety net). */
void drive_refresh(drive_status_t *status);

/* Logging events from the ISRs (task context consumer). */
QueueHandle_t drive_events(void);

/* Edges seen on the select line (start-up guard). */
uint32_t drive_select_edges(void);

/* STEP pulses ignored because the drive was not armed/selected. */
uint32_t drive_ignored_steps(void);

#define DRIVE_MEDIA_CHANGE_MS   700

/*
 * Swap the disk: runs swap(arg) under the drive lock, but only while our
 * drive is not selected, then signals a disk change. present: a disk is
 * inserted afterwards. Returns false (nothing done) while selected.
 */
bool drive_swap_media(void (*swap)(void *), void *arg, bool present);

/* Armed by the start-up guard. */
bool drive_is_armed(void);

/* Current virtual cylinder. ISR/IRAM safe. */
int drive_cylinder(void);
