/*
 * Virtual read-only Shugart drive: selection, head position and outputs.
 *
 * All output decisions are made here, in ISR context, from the live input
 * levels, so they never wait for logging or a background task:
 *
 *   active  = armed && selected (EMU_SELECT_LINE LOW)
 *   disk    = a disk is inserted and no disk change is being signalled
 *   TRACK0  = active && cylinder == 0
 *   WPROT   = active && !changing           (read-only disk)
 *   INDEX   = active && disk && MOTOR on
 *   RDATA   = active && disk && MOTOR on && WGATE inactive
 *   READY, DSKCHG: never driven
 *
 * Disk change: for DRIVE_MEDIA_CHANGE_MS after a swap WPROT is released
 * and INDEX/RDATA stay off, as with an open drive door. TOS notices a
 * disk change through that write-protect transition.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Highest virtual head position; later per drive/image configuration. */
#define DRIVE_MAX_TRACK 79

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

/* Configure the inputs and install the interrupts (drive not armed). */
void drive_init(void);

/* Arm the drive (once, after the start-up guard) and apply the outputs. */
void drive_arm(drive_status_t *status);

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
