/*
 * .ST image validation (plain C, also built in the host tests).
 *
 * A .ST file is a raw sector dump without a header, so its geometry
 * follows from the size. The emulator currently supports 80 cylinders,
 * 9 sectors of 512 bytes, 1 or 2 sides (368640 / 737280 bytes).
 */
#pragma once

#include <stdint.h>

#define ST_MAX_SIZE         819200u     /* 800 KiB = one flash slot */

typedef enum {
    ST_OK,
    ST_INVALID,             /* not a .ST image at all */
    ST_TOO_LARGE,           /* more than ST_MAX_SIZE */
    ST_UNSUPPORTED,         /* plausible .ST, geometry not supported yet */
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
