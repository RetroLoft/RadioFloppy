/*
 * Entry points of the individual hardware tests. app_main() in main.c
 * selects which one runs.
 */
#pragma once

/* Detach the native USB PHY from GPIO19/GPIO20 (see board.c). */
void usb_phy_release_pins(void);

/* Drive the six Shugart outputs LOW = all ULN2003A outputs released. */
void shugart_outputs_release(void);

/* Push buttons (GPIO19 left, GPIO8 right) + 0.5 s start-up beep. */
void button_test_run(void);

/* Shugart output -> input loopback scanner (jumper on J1). */
void loopback_test_run(void);

/* Read-only floppy emulation of a .ST image from the external flash (drive B:). */
void floppy_emu_run(void);
