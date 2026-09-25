#!/bin/sh
# Build and run the host tests (flash layout, image store, .ST validation, settings).
set -e
cd "$(dirname "$0")"
CFLAGS="-std=gnu17 -O1 -Wall -Wextra -Wno-unused-parameter -I../../main -Istubs -I."

gcc $CFLAGS -DIMAGE_STORE_NO_LOCK -o test_image_store test_image_store.c mock_ext_flash.c \
    ../../main/image_store.c
gcc $CFLAGS -o test_st_image test_st_image.c ../../main/st_image.c
gcc $CFLAGS -o test_mfm test_mfm.c ../../main/mfm_track.c
gcc $CFLAGS -DSETTINGS_DEFAULT_HOSTNAME='"RadioFloppy"' -DSETTINGS_DEFAULT_SSID='"DefaultNet"' \
    -DSETTINGS_DEFAULT_PASS='"password1"' -o test_settings test_settings.c mock_ext_flash.c \
    ../../main/settings.c
gcc $CFLAGS -o test_title test_title.c ../../main/disk_title.c ../../main/oled_gfx.c

echo "== image store =="; ./test_image_store
echo "== .ST validation =="; ./test_st_image
echo "== MFM tracks and flux timing =="; ./test_mfm
echo "== titles and OLED wrap =="; ./test_title
echo "== settings =="; ./test_settings
