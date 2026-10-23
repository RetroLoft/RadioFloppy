/*
 * Paths on the SD card from API requests. See sd_path.h.
 */
#include <stdio.h>
#include <string.h>

#include "sd_path.h"

static bool name_ok(const char *p, size_t len)
{
    if (len == 0 || len > SD_NAME_MAX || p[0] == '.' || p[0] == ' ' ||
        p[len - 1] == '.' || p[len - 1] == ' ') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c > 0x7e || strchr("\\/:*?\"<>|", c)) {
            return false;
        }
    }
    return true;
}

bool sd_name_valid(const char *name)
{
    return name_ok(name, strnlen(name, SD_NAME_MAX + 1));
}

bool sd_relpath_valid(const char *rel)
{
    size_t n = strnlen(rel, SD_REL_MAX + 1);

    if (n > SD_REL_MAX) {
        return false;
    }
    for (const char *p = rel; *p;) {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!name_ok(p, len)) {
            return false;               /* also "//" and '/' at the start */
        }
        if (!end) {
            break;
        }
        if (!end[1]) {
            return false;               /* '/' at the end */
        }
        p = end + 1;
    }
    return true;
}

bool sd_path_join(char *out, size_t size, const char *mount, const char *root, const char *rel)
{
    int n = snprintf(out, size, "%s%s%s%s%s", mount, root[0] ? "/" : "", root,
                     rel[0] ? "/" : "", rel);
    return n > 0 && (size_t)n < size;
}

static int hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool sd_url_decode(char *s)
{
    char *w = s;

    for (const char *r = s; *r; r++) {
        int c = (unsigned char)*r;
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            int hi = hex(r[1]), lo = hi < 0 ? -1 : hex(r[2]);
            if (lo < 0) {
                return false;
            }
            c = hi * 16 + lo;
            r += 2;
        }
        if (c < 0x20) {
            return false;               /* NUL and control characters */
        }
        *w++ = (char)c;
    }
    *w = 0;
    return true;
}
