#!/bin/sh
# Build and run the host tests (flash layout, slot store, .ST validation).
set -e
cd "$(dirname "$0")"
CFLAGS="-std=gnu17 -O1 -Wall -Wextra -Wno-unused-parameter -I../../main -Istubs -I."

gcc $CFLAGS -o test_slot_store test_slot_store.c mock_ext_flash.c \
    ../../main/slot_store.c ../../main/legacy_catalog.c
gcc $CFLAGS -o test_st_image test_st_image.c ../../main/st_image.c
gcc $CFLAGS -o test_mfm test_mfm.c ../../main/mfm_track.c

echo "== slot store =="; ./test_slot_store
echo "== .ST validation =="; ./test_st_image
echo "== MFM tracks and flux timing =="; ./test_mfm
