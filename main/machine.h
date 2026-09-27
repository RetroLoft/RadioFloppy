/*
 * The computer RadioFloppy is connected to (setting "machine").
 *
 * One profile per computer family. Only the Atari ST family is supported
 * for now; the other profiles can already be chosen and stored, but then
 * the floppy interface stays off (outputs released). Later a profile will
 * also select the floppy interface, the image formats, the blank disks
 * that can be created and the track encoder / write decoder.
 *
 * The numeric values are stored in the settings (external flash): never
 * renumber them; add new ones at the end.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    MACHINE_ATARI = 0,          /* Atari 16-bit (ST/STE/Mega ST): default */
    MACHINE_AMIGA = 1,          /* Commodore Amiga: reserved */
    MACHINE_DOS = 2,            /* IBM PC / DOS: reserved */
    MACHINE_COUNT
} machine_t;

typedef struct {
    const char *id;             /* stable identifier for the API: "ATARI", ... */
    const char *name;           /* for people: "Atari 16-bit", ... */
    bool supported;             /* emulation available in this firmware */
} machine_profile_t;

/* Profile of m; an unknown value gives the Atari profile. */
const machine_profile_t *machine_profile(int m);

/* machine_t for an id ("ATARI", "AMIGA", "DOS"), or -1. */
int machine_from_id(const char *id);

/* The profile this boot runs with (set once at start-up from the settings;
 * a changed setting takes effect after a restart). */
void machine_set_active(int m);
int machine_active(void);
