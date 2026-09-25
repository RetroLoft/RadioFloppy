/*
 * Monochrome framebuffer and text for the SSD1306. See oled_gfx.h.
 */
#include <string.h>

#include "oled_font.h"
#include "oled_gfx.h"

uint8_t oled_fb[OLED_PAGES * OLED_WIDTH];

void oled_gfx_clear(void)
{
    memset(oled_fb, 0, sizeof(oled_fb));
}

void oled_gfx_pixel(int x, int y, int on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    uint8_t *b = &oled_fb[(y / 8) * OLED_WIDTH + x];
    if (on) {
        *b |= 1 << (y % 8);
    } else {
        *b &= ~(1 << (y % 8));
    }
}

void oled_gfx_text(int x, int y, const char *s)
{
    oled_gfx_text_tall(x, y, OLED_FONT_HEIGHT, s);
}

void oled_gfx_text_tall(int x, int y, int height, const char *s)
{
    for (; *s; s++, x += OLED_FONT_WIDTH + 1) {
        char c = *s;
        if (c < OLED_FONT_FIRST || c > OLED_FONT_LAST) {
            c = '?';
        }
        const uint8_t *glyph = oled_font5x7[c - OLED_FONT_FIRST];
        for (int col = 0; col < OLED_FONT_WIDTH; col++) {
            for (int row = 0; row < height; row++) {
                int src = row * OLED_FONT_HEIGHT / height;     /* font row */
                if (glyph[col] & (1 << src)) {
                    oled_gfx_pixel(x + col, y + row, 1);
                }
            }
        }
    }
}

int oled_gfx_text_wrapped(int y, int max_lines, int height, int pitch, const char *s)
{
    return oled_gfx_text_wrapped_ex(y, max_lines, height, pitch, OLED_COLS, s);
}

int oled_gfx_text_wrapped_ex(int y, int max_lines, int height, int pitch, int last_cols,
                             const char *s)
{
    char line[OLED_COLS + 1];
    int lines = 0;

    if (last_cols < 4 || last_cols > OLED_COLS) {
        last_cols = OLED_COLS;
    }
    while (*s && lines < max_lines) {
        size_t cols = lines == max_lines - 1 ? (size_t)last_cols : OLED_COLS;
        while (*s == ' ') {
            s++;
        }
        size_t len = strlen(s);
        size_t take = len;
        if (len > cols) {
            /* Break at the last space that still fits, else split the word. */
            take = cols;
            for (size_t i = cols; i > 0; i--) {
                if (s[i] == ' ') {
                    take = i;
                    break;
                }
            }
        }
        if (lines == max_lines - 1 && len > cols) {
            /* Last line and the text does not fit: end with "...", cut at
             * a word boundary when that keeps most of the line. */
            take = cols - 3;
            for (size_t i = take; i > take / 2; i--) {
                if (s[i] == ' ') {
                    take = i;
                    break;
                }
            }
            while (take && s[take - 1] == ' ') {
                take--;
            }
            memcpy(line, s, take);
            memcpy(line + take, "...", 4);
            oled_gfx_text_tall(0, y + lines * pitch, height, line);
            return lines + 1;
        }
        memcpy(line, s, take);
        line[take] = 0;
        oled_gfx_text_tall(0, y + lines * pitch, height, line);
        s += take;
        lines++;
    }
    return lines;
}
