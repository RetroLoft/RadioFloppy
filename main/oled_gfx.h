/*
 * Monochrome framebuffer for the SSD1306 (plain C, also built in the host
 * tests). Layout as the controller's page memory: byte [page][x], bit n =
 * row page * 8 + n.
 */
#pragma once

#include <stdint.h>

/* Display resolution: change here for a 128x64 module. */
#define OLED_WIDTH      128
#define OLED_HEIGHT     32
#define OLED_PAGES      (OLED_HEIGHT / 8)

extern uint8_t oled_fb[OLED_PAGES * OLED_WIDTH];

void oled_gfx_clear(void);
void oled_gfx_pixel(int x, int y, int on);

#define OLED_CHAR_W     6                       /* 5x7 glyph + 1 column gap */
#define OLED_LINE_H     8
#define OLED_COLS       (OLED_WIDTH / OLED_CHAR_W)  /* 21 on a 128 px display */
#define OLED_ROWS       (OLED_HEIGHT / OLED_LINE_H) /* 4 on 32 px */

/* 5x7 text, 6 pixels per character; clipped at the display edge. */
void oled_gfx_text(int x, int y, const char *s);

/* 5x7 text stretched to `height` pixel rows (nearest neighbour), same width. */
void oled_gfx_text_tall(int x, int y, int height, const char *s);

/*
 * Text wrapped at spaces over at most max_lines lines of OLED_COLS
 * characters, starting at pixel row y, glyphs `height` rows tall (7 =
 * normal) and lines `pitch` rows apart; a word longer than a line is
 * split. If the text needs more lines, the last one ends with "...".
 * Returns the number of lines used.
 */
int oled_gfx_text_wrapped(int y, int max_lines, int height, int pitch, const char *s);

/* As above, but the last line holds at most last_cols characters (room
 * for an icon at the right). */
int oled_gfx_text_wrapped_ex(int y, int max_lines, int height, int pitch, int last_cols,
                             const char *s);
