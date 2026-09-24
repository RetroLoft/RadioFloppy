# Floppy images embedded in the firmware. RETROLOFT_TEST_720K.ST is part of
# the repository; other images (e.g. commercial software) are only embedded
# when present locally in images/ and are not committed.
set(images "../images/RETROLOFT_TEST_720K.ST")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../images/CRYSTAL_CASTLES.ST")
    list(APPEND images "../images/CRYSTAL_CASTLES.ST")
    set(have_crystal_castles 1)
endif()

idf_component_register(SRCS "main.c"
                            "board.c"
                            "button_test.c"
                            "loopback_test.c"
                            "floppy_app.c"
                            "drive_emu.c"
                            "flux_stream.c"
                            "mfm_track.c"
                            "disk_image.c"
                            "ext_flash.c"
                            "image_store.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES ${images}
                       PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_driver_spi esp_rom
                                     esp_timer esp_psram heap spi_flash)

if(have_crystal_castles)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CRYSTAL_CASTLES=1)
endif()
