/*
 * Host tests for SD card paths from API requests (main/sd_path.c): names
 * and relative paths that are allowed and refused, joining, URL decoding.
 */
#include <stdio.h>
#include <string.h>

#include "sd_path.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
    static const char *good_names[] = {
        "L", "Leisure Suit Larry in the Land of the Lounge Lizards (1987)(Sierra)[codes on disk].st",
        "Salomon's Key (1986)(U.S. Gold)[b].st", "a#b$c%d@e~f=g;h{i}j^k`l", "x.st", "My Floppies",
    };
    static const char *bad_names[] = {
        "", ".", "..", ".hidden", " lead", "trail ", "name.", "a/b", "a\\b", "c:d", "s*", "q?", "q\"",
        "<x>", "p|q", "tab\there", "\xc3\xa9t\xc3\xa9",
    };
    for (size_t i = 0; i < sizeof(good_names) / sizeof(good_names[0]); i++) {
        if (!sd_name_valid(good_names[i])) { printf("  name wrongly refused: '%s'\n", good_names[i]); failures++; }
    }
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
        if (sd_name_valid(bad_names[i])) { printf("  name wrongly accepted: '%s'\n", bad_names[i]); failures++; }
    }
    char longname[SD_NAME_MAX + 2];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[SD_NAME_MAX] = 0;
    CHECK(sd_name_valid(longname));
    longname[SD_NAME_MAX] = 'a';
    longname[SD_NAME_MAX + 1] = 0;
    CHECK(!sd_name_valid(longname));

    CHECK(sd_relpath_valid(""));
    CHECK(sd_relpath_valid("L"));
    CHECK(sd_relpath_valid("L/Larry [a].st"));
    CHECK(sd_relpath_valid("A/B/C"));
    CHECK(!sd_relpath_valid("/L"));
    CHECK(!sd_relpath_valid("L/"));
    CHECK(!sd_relpath_valid("L//x"));
    CHECK(!sd_relpath_valid("L/../x"));
    CHECK(!sd_relpath_valid("../x"));
    CHECK(!sd_relpath_valid("L/./x"));
    char longpath[SD_REL_MAX + 2];
    for (int i = 0; i < SD_REL_MAX + 1; i++) longpath[i] = (i % 10 == 9) ? '/' : 'a';
    longpath[SD_REL_MAX + 1] = 0;
    CHECK(!sd_relpath_valid(longpath));

    char out[64];
    CHECK(sd_path_join(out, sizeof(out), "/sd", "", "") && strcmp(out, "/sd") == 0);
    CHECK(sd_path_join(out, sizeof(out), "/sd", "My Floppies", "") && strcmp(out, "/sd/My Floppies") == 0);
    CHECK(sd_path_join(out, sizeof(out), "/sd", "My Floppies", "L/x.st") && strcmp(out, "/sd/My Floppies/L/x.st") == 0);
    CHECK(sd_path_join(out, sizeof(out), "/sd", "", "L") && strcmp(out, "/sd/L") == 0);
    CHECK(!sd_path_join(out, 10, "/sd", "My Floppies", "L"));

    char q1[] = "L%2FLeisure+Suit+Larry%20%5Ba%5D.st";
    CHECK(sd_url_decode(q1) && strcmp(q1, "L/Leisure Suit Larry [a].st") == 0);
    char q2[] = "bad%2";
    CHECK(!sd_url_decode(q2));
    char q3[] = "nul%00x";
    CHECK(!sd_url_decode(q3));
    char q4[] = "%2e%2e";
    CHECK(sd_url_decode(q4) && !sd_relpath_valid(q4));      /* decoded "..": still refused */

    printf(failures ? "FAILED (%d)\n" : "SD paths OK\n", failures);
    return failures != 0;
}
