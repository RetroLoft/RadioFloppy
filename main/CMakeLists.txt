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
                       PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_driver_spi esp_rom
                                     esp_timer esp_psram heap spi_flash)

