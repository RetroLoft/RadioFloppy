/*
 * Host tests for disk titles (main/disk_title.c) and the OLED text wrap.
 *   tests/host/run.sh
 */
#include <stdio.h>
#include <string.h>

#include "disk_title.h"
#include "oled_gfx.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int title_is(const char *fn, const char *want)
{
    char t[DISK_TITLE_SIZE];
    disk_title_from_filename(fn, t);
    if (strcmp(t, want) != 0) {
        printf("  got \"%s\" for %s\n", t, fn);
        return 0;
    }
    return 1;
}

int main(void)
{
    printf("titles\n");
    CHECK(title_is("Leisure Suit Larry in the Land of the Lounge Lizards (1987)(Sierra)[codes on disk].st",
                   "Leisure Suit Larry in the Land of the Lounge Lizards"));
    CHECK(title_is("/share/ST/V/Virus (1988)(Braben, D.J.)[cr Replicants][t].st", "Virus"));
    CHECK(title_is("C:\\games\\Crystal Castles (1986)(Atari).st", "Crystal Castles"));
    CHECK(title_is("Space Gun (1991)(Ocean)(Disk 2 of 2)[t].st", "Space Gun (Disk 2/2)"));
    CHECK(title_is("WWF Wrestlemania (1991)(Ocean)(Disk 1 of 2)(Disk A)[cr Elite].st",
                   "WWF Wrestlemania (Disk A)"));
    CHECK(title_is("Leisure Suit Larry 2 - Leisure Suit Larry Goes Looking For Love in Several "
                   "Wrong Places (1988)(Sierra)(Disk 1 of 3).st",
                   "Leisure Suit Larry 2 - Leisure Suit Larry (Disk 1/3)"));
    CHECK(title_is("Nebulus.st", "Nebulus"));
    CHECK(title_is("(1990)(Test).st", "(1990)(Test)"));
    CHECK(title_is(".st", ".st"));
    CHECK(title_is("", "Untitled"));

    printf("OLED wrap\n");
    oled_gfx_clear();
    CHECK(oled_gfx_text_wrapped(0, 2, 11, 16, "Crystal Castles") == 1);
    CHECK(oled_gfx_text_wrapped(0, 2, 11, 16, "Leisure Suit Larry in the Land of the Lounge Lizards") == 2);
    CHECK(oled_gfx_text_wrapped_ex(0, 2, 11, 16, 19, "Leisure Suit Larry in the Land of the Lounge Lizards") == 2);
    CHECK(oled_gfx_text_wrapped_ex(0, 2, 11, 16, 19, "Crystal Castles") == 1);

    printf("\n%s\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED");
    return failures != 0;
}
