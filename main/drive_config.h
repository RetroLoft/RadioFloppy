/*
 * Emulated drive configuration (compile time for now; later via the web
 * interface). Replaces the DS0/DS1 jumpers of a real floppy drive.
 */
#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "board_pins.h"

#define EMU_DS0 PIN_FDD_DS0     /* Shugart pin 10, GPIO4 */
#define EMU_DS1 PIN_FDD_DS1     /* Shugart pin 12, GPIO5 */

// Verander alleen deze regel om de drive-select-ingang te kiezen:
#define EMU_SELECT_LINE EMU_DS1

#if EMU_SELECT_LINE != EMU_DS0 && EMU_SELECT_LINE != EMU_DS1
#error "EMU_SELECT_LINE must be EMU_DS0 or EMU_DS1"
#endif

#define EMU_SELECT_NAME ((EMU_SELECT_LINE) == EMU_DS0 ? "DS0" : "DS1")

/*
 * The one place that decides whether the Atari is talking to our drive.
 * Drive select is active low. IRAM-safe (CONFIG_GPIO_CTRL_FUNC_IN_IRAM),
 * so it is also used from the GPIO ISR.
 */
static inline bool emulator_is_selected(void)
{
    return gpio_get_level(EMU_SELECT_LINE) == 0;
}
