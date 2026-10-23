/*
 * Entry points of the individual hardware tests. app_main() in main.c
 * selects which one runs.
 */
#pragma once

/* Detach the native USB PHY from GPIO19/GPIO20 (see board.c). */
void usb_phy_release_pins(void);

/* Drive the six Shugart outputs LOW = all ULN2003A outputs released. */
void shugart_outputs_release(void);

/* Keep the SPI chip selects (NOR flash, SD card) HIGH = deselected. */
void spi_cs_release(void);

/* Shugart output -> input loopback scanner (jumper on J1). */
void loopback_test_run(void);

/* The floppy emulator (drive B:), or WiFi setup mode. */
void floppy_emu_run(void);
