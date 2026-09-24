/*
 * Second-stage bootloader hook: release the Shugart outputs immediately.
 *
 * The six Shugart outputs are driven through a ULN2003A: a HIGH GPIO pulls
 * the line on the floppy connector to GND. Drive these GPIOs LOW before the
 * bootloader does anything else, so a connected Atari never sees a spurious
 * INDEX/TRACK0/WPROT/RDATA/READY/DSKCHG while the app is being loaded.
 * The application configures the same pins again as its first action.
 *
 * Keep in sync with main/board_pins.h (not usable from the bootloader).
 */
#include <stdint.h>

#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"

/* Referenced by the bootloader link (-u) so this component is kept. */
void bootloader_hooks_include(void)
{
}

void bootloader_before_init(void)
{
    /* INDEX, TRACK0, WPROT, RDATA, READY, DSKCHG */
    static const uint32_t shugart_outputs[] = { 1, 2, 42, 41, 40, 39 };

    for (int i = 0; i < sizeof(shugart_outputs) / sizeof(shugart_outputs[0]); i++) {
        uint32_t pin = shugart_outputs[i];

        gpio_ll_set_level(&GPIO, pin, 0);
        esp_rom_gpio_pad_select_gpio(pin);
        esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
        gpio_ll_pullup_dis(&GPIO, pin);
        gpio_ll_output_enable(&GPIO, pin);
    }
}
