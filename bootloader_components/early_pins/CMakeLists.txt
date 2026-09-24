# Bootloader hook: drive the Shugart output GPIOs LOW as early as possible.
idf_component_register(SRCS "early_pins.c"
                       REQUIRES hal soc esp_rom)
