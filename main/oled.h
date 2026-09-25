/*
 * Optional SSD1306 OLED on I2C (SDA GPIO47, SCL GPIO21, address 0x3C).
 * Resolution: OLED_WIDTH / OLED_HEIGHT in oled_gfx.h.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "oled_gfx.h"

#define OLED_SDA_GPIO   47
#define OLED_SCL_GPIO   21
#define OLED_I2C_ADDR   0x3C

/* Detect and initialise the display. Not fatal: false if absent/failed. */
bool oled_init(void);

/* Send the framebuffer (oled_fb) to the display. */
esp_err_t oled_flush(void);

/* First test: "RadioFloppy" / "OLED test OK". */
void oled_show_test(void);

/*
 * Show the title of the active disk and keep it up to date: a low-priority
 * task redraws the display whenever the active disk changes.
 */
void oled_start_disk_title(void);

/* Show the network name (host name) and IP address for OLED_INFO_MS, then
 * the title again. Any task; does not wait for I2C. */
void oled_show_network(void);

#define OLED_INFO_MS    10000

/* Two lines in the large font; stays until the next restart (used just
 * before one). Any task. */
void oled_show_message(const char *line1, const char *line2);

/* Four lines in the small font (setup mode, no title task running). */
void oled_show_lines(const char *l0, const char *l1, const char *l2, const char *l3);
