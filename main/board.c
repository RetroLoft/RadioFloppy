/*
 * Board-level initialisation shared by all firmware modes.
 */
#include "driver/gpio.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/usb_serial_jtag_reg.h"

#include "board_pins.h"
#include "tests.h"

/*
 * GPIO19/GPIO20 are the native USB D-/D+ pads and are used as normal GPIOs
 * on this PCB (SW_NEXT, LED_STATUS). With the USB Serial/JTAG disabled in sdkconfig its bus clock
 * is gated, so the pad-disable done by the GPIO driver does not reach the
 * register and the PHY (with its D+ pull-up) stays attached to the pins.
 * Enable the clock briefly, detach the pads and gate the clock again.
 */
void usb_phy_release_pins(void)
{
    int __DECLARE_RCC_ATOMIC_ENV __attribute__((unused));

    usb_serial_jtag_ll_enable_bus_clock(true);
    usb_serial_jtag_ll_phy_enable_pad(false);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
    usb_serial_jtag_ll_enable_bus_clock(false);
}

/*
 * Configure the six Shugart output GPIOs as outputs driving LOW. The
 * ULN2003A then keeps all floppy connector outputs released (not pulled
 * to GND). The bootloader hook in bootloader_components/early_pins has
 * already done the same; this makes it explicit for the application.
 */
void shugart_outputs_release(void)
{
    static const gpio_num_t pins[] = {
        PIN_FDD_INDEX, PIN_FDD_TRK0, PIN_FDD_WPROT,
        PIN_FDD_RDATA, PIN_FDD_READY, PIN_FDD_DSKCHG,
    };
    uint64_t mask = 0;

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_set_level(pins[i], 0);
        mask |= 1ULL << pins[i];
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_set_level(pins[i], 0);
    }
}

/*
 * The NOR flash and the SD card module share one SPI bus: a device whose
 * CS is not HIGH could answer while the other one is used. The SPI driver
 * takes over PIN_NOR_CS; PIN_SD_CS stays a plain output (HIGH) until the
 * SD card is used. The bootloader hook does the same.
 */
void spi_cs_release(void)
{
    gpio_set_level(PIN_NOR_CS, 1);
    gpio_set_level(PIN_SD_CS, 1);
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_NOR_CS) | (1ULL << PIN_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_NOR_CS, 1);
    gpio_set_level(PIN_SD_CS, 1);
}
