/*
 * Machine profiles. See machine.h.
 */
#include <string.h>

#include "machine.h"

static const machine_profile_t profiles[MACHINE_COUNT] = {
    [MACHINE_ATARI] = { .id = "ATARI", .name = "Atari 16-bit",      .supported = true },
    [MACHINE_AMIGA] = { .id = "AMIGA", .name = "Commodore Amiga",   .supported = false },
    [MACHINE_DOS]   = { .id = "DOS",   .name = "IBM PC / DOS",      .supported = false },
};

const machine_profile_t *machine_profile(int m)
{
    return m >= 0 && m < MACHINE_COUNT ? &profiles[m] : &profiles[MACHINE_ATARI];
}

static int active = MACHINE_ATARI;

void machine_set_active(int m)
{
    active = m >= 0 && m < MACHINE_COUNT ? m : MACHINE_ATARI;
}

int machine_active(void)
{
    return active;
}

int machine_from_id(const char *id)
{
    for (int i = 0; i < MACHINE_COUNT; i++) {
        if (strcmp(id, profiles[i].id) == 0) {
            return i;
        }
    }
    return -1;
}
