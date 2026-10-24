#!/bin/sh
# Build and run the host tests (.ST validation, MFM, settings, HFE, SD paths, disk copy).
set -e
cd "$(dirname "$0")"
CFLAGS="-std=gnu17 -O1 -Wall -Wextra -Wno-unused-parameter -I../../main -Istubs -I."

gcc $CFLAGS -o test_st_image test_st_image.c ../../main/st_image.c
gcc $CFLAGS -o test_mfm test_mfm.c ../../main/mfm_track.c
gcc $CFLAGS -DSETTINGS_NO_LOCK -DSETTINGS_DEFAULT_HOSTNAME='"RadioFloppy"' -DSETTINGS_DEFAULT_SSID='"DefaultNet"' \
    -DSETTINGS_DEFAULT_PASS='"password1"' -o test_settings test_settings.c mock_ext_flash.c \
    ../../main/settings.c ../../main/machine.c ../../main/sd_path.c
gcc $CFLAGS -o test_st_format test_st_format.c ../../main/st_format.c ../../main/st_image.c \
    ../../main/mfm_track.c
gcc $CFLAGS -o test_mfm_write test_mfm_write.c ../../main/mfm_track.c
gcc $CFLAGS -o test_hfe test_hfe.c ../../main/hfe.c
gcc $CFLAGS -o test_sd_path test_sd_path.c ../../main/sd_path.c
gcc $CFLAGS -DDISK_COPY_NO_LOCK -o test_disk_copy test_disk_copy.c mock_ext_flash.c ../../main/disk_copy.c
gcc $CFLAGS -o test_title test_title.c ../../main/disk_title.c ../../main/oled_gfx.c

echo "== .ST validation =="; ./test_st_image
echo "== MFM tracks and flux timing =="; ./test_mfm
echo "== titles and OLED wrap =="; ./test_title
echo "== settings =="; ./test_settings
echo "== blank disk =="; ./test_st_format
echo "== MFM write decoding =="; ./test_mfm_write
echo "== HFE =="; ./test_hfe ~/Downloads/road_runner_us_gold/*.hfe ${HFE_TEST_FILES:-}
echo "== SD paths =="; ./test_sd_path
echo "== disk copy =="; ./test_disk_copy
