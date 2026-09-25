/*
 * Floppy stepper-motor sound: a short click on the active buzzer for every
 * STEP pulse our drive processes.
 */
#pragma once

#include <stdbool.h>

#define STEP_SOUND_ENABLED      1
#define STEP_SOUND_PULSE_MS     3   /* buzzer on per STEP (try 2, 3 or 5) */
#define STEP_SOUND_MIN_OFF_MS   1   /* forced silence between two clicks */

/* Setting "buzzer": false silences step and button clicks (any time). */
void step_sound_set_enabled(bool on);

/* Buzzer output LOW, timer created. Call before the drive interrupts. */
void step_sound_init(void);

/* ISR: our drive processed a STEP pulse. Never blocks. */
void step_sound_step_isr(void);

/* Task context: one click (e.g. to acknowledge a button press). */
void step_sound_click(void);

/* ISR: our drive is no longer active: buzzer off, pending click dropped. */
void step_sound_stop_isr(void);
