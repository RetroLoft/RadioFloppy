/*
 * Emulated drive configuration: which drive-select line the emulator
 * answers to, like the DS0/DS1 jumper of a real floppy drive. Chosen in
 * the settings (external flash, settings.h), set once at start-up by
 * drive_set_select_line() before drive_init(), never changed while the
 * drive runs.
 */
#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "board_pins.h"

#define EMU_DS0 PIN_FDD_DS0     /* Shugart pin 10, GPIO4 */
#define EMU_DS1 PIN_FDD_DS1     /* Shugart pin 12, GPIO5 */

/* The select line in use (DRAM, read by the GPIO ISR). Default DS1 = B:. */
extern gpio_num_t emu_select_line;
#define EMU_SELECT_LINE emu_select_line

#define EMU_SELECT_NAME ((EMU_SELECT_LINE) == EMU_DS0 ? "DS0" : "DS1")
#define EMU_DRIVE_NAME  ((EMU_SELECT_LINE) == EMU_DS0 ? "A:" : "B:")

/* ds: 0 = DS0 (drive A:), 1 = DS1 (drive B:). Only before drive_init(). */
static inline void drive_set_select_line(int ds)
{
    emu_select_line = ds == 0 ? EMU_DS0 : EMU_DS1;
}

/*
 * The one place that decides whether the Atari is talking to our drive.
 * Drive select is active low. IRAM-safe (CONFIG_GPIO_CTRL_FUNC_IN_IRAM),
 * so it is also used from the GPIO ISR.
 */
static inline bool emulator_is_selected(void)
{
    return gpio_get_level(EMU_SELECT_LINE) == 0;
}
