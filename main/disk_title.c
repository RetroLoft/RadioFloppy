/*
 * Disk titles from client file names. See disk_title.h.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "disk_title.h"

/* Append c to out (length *o, capacity max), collapsing spaces. */
static void put(char *out, size_t *o, size_t max, char c)
{
    if (c == ' ' && (*o == 0 || out[*o - 1] == ' ')) {
        return;
    }
    if (*o < max) {
        out[(*o)++] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? c : '_';
    }
}

static size_t trim(char *s, size_t o)
{
    while (o && s[o - 1] == ' ') {
        o--;
    }
    s[o] = 0;
    return o;
}

void disk_title_from_filename(const char *fn, char out[DISK_TITLE_SIZE])
{
    const char *base = fn;
    for (const char *p = fn; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        len = dot - base;
    }

    /* Title without TOSEC tags; "(Disk 1 of 2)" is kept apart as "(Disk 1/2)". */
    char title[256];
    char disk[24] = "";
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = base[i];
        char close = c == '(' ? ')' : c == '[' ? ']' : 0;
        const char *end = close ? memchr(base + i, close, len - i) : NULL;
        if (end) {
            unsigned n, of;
            char tag[32];
            size_t tl = end - (base + i) - 1;
            if (c == '(' && tl < sizeof(tag)) {
                memcpy(tag, base + i + 1, tl);
                tag[tl] = 0;
                if (sscanf(tag, "Disk %u of %u", &n, &of) == 2) {
                    snprintf(disk, sizeof(disk), "(Disk %u/%u)", n, of);
                } else if (strncasecmp(tag, "Disk ", 5) == 0) {
                    snprintf(disk, sizeof(disk), "(%.20s)", tag);
                }
            }
            i = end - base;
            continue;
        }
        put(title, &o, sizeof(title) - 1, c);
    }
    o = trim(title, o);
    if (o == 0) {
        /* Nothing but tags: use the plain base name. */
        for (size_t i = 0; i < len; i++) {
            put(title, &o, sizeof(title) - 1, base[i]);
        }
        o = trim(title, o);
        disk[0] = 0;
    }
    if (o == 0) {
        strcpy(title, "Untitled");
    }

    /* Shorten the title, never the disk number. */
    size_t room = DISK_TITLE_MAX - (disk[0] ? strlen(disk) + 1 : 0);
    size_t tlen = strlen(title);
    if (tlen > room) {
        tlen = trim(title, room);
    }
    snprintf(out, DISK_TITLE_SIZE, "%.*s%s%s", (int)tlen, title, disk[0] ? " " : "", disk);
}
