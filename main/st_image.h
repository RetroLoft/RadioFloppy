/*
 * .ST image validation (plain C, also built in the host tests).
 *
 * A .ST file is a raw sector dump without a header, so its geometry
 * follows from the size (unambiguous for the supported range); the boot
 * sector (BPB) is only checked and reported, never a reason to refuse. Supported:
 * 79..84 cylinders, 9..11 sectors of 512 bytes, 1 or 2 sides, at most
 * ST_MAX_SIZE bytes (the image library limit, RF_MAX_IMAGE_SIZE; the
 * largest supported geometry, 84/2/11, is 946 176 bytes).
 */
#pragma once

#include <stdint.h>

#define ST_MAX_SIZE         (1536u * 1024u) /* = RF_MAX_IMAGE_SIZE */
#define ST_MIN_CYLS         79      /* 79: track 79 is played unformatted */
#define ST_MAX_CYLS         84
#define ST_MIN_SECTORS      9
#define ST_MAX_SECTORS      11

typedef enum {
    ST_OK,
    ST_INVALID,             /* not a .ST image at all */
    ST_TOO_LARGE,           /* more than ST_MAX_SIZE */
    ST_UNSUPPORTED,         /* plausible .ST, geometry not supported */
} st_result_t;

typedef struct {
    int cylinders, heads, sectors;  /* geometry used by the emulator */
    int bpb_ok;                     /* boot sector BPB present and consistent */
    char detail[160];               /* human readable reason / remark */
} st_info_t;

/* Check the size alone (before any data has been received). */
st_result_t st_check_size(uint32_t size, st_info_t *info);

/* Full check with the data (size must already have passed). */
st_result_t st_check_image(const uint8_t *data, uint32_t size, st_info_t *info);
