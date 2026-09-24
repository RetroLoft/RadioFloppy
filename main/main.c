/*
 * FloppyEmulator-ESP32S3 - hardware bring-up firmware.
 *
 * Runs one of the hardware tests from tests.h; change the call below to
 * select another test.
 */
#include "tests.h"

void app_main(void)
{
    /* First: keep every Shugart output released (a real Atari may be on). */
    shugart_outputs_release();
    usb_phy_release_pins();

    floppy_emu_run();
}
