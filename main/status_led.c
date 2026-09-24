/*
 * On-board addressable RGB LED, driven by its own RMT TX channel (bytes
 * encoder, GRB order). See status_led.h.
 *
 * A low-priority task polls selection and MOTOR every STATUS_POLL_MS and only
 * sends to the LED when the state changes, so the floppy interrupts and
 * the flux stream are never involved. The channel uses the same 10 MHz
 * resolution as the flux channels (the RMT group clock is shared).
 */
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

#include "board_pins.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "flux_stream.h"
#include "status_led.h"

#define STATUS_POLL_MS  20

/* WS2812 bit timing at 10 MHz (0.1 us per tick), as ESP-IDF's led_strip:
 * 0 = 0.3/0.9 us, 1 = 0.9/0.3 us. */
#define T0H 3
#define T0L 9
#define T1H 9
#define T1L 3

static rmt_channel_handle_t led_chan;
static rmt_encoder_handle_t led_enc;

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t grb[3] = { g, r, b };
    const rmt_transmit_config_t tx = { .loop_count = 0 };

    esp_err_t e1 = rmt_transmit(led_chan, led_enc, grb, sizeof(grb), &tx);
    esp_err_t e2 = rmt_tx_wait_all_done(led_chan, pdMS_TO_TICKS(10));
    if (e1 != ESP_OK || e2 != ESP_OK) {
        printf("LED DIAG: transmit %s, wait %s\n", esp_err_to_name(e1), esp_err_to_name(e2));
    }
    /* The idle LOW until the next update (>= 20 ms) is the latch/reset. */
}

static void status_led_task(void *arg)
{
    int shown = -1;

    while (true) {
        /* Our drive selected (EMU_SELECT_LINE) and MOTOR active (LOW);
         * armed, so an unpowered Atari (all lines LOW) does not count. */
        bool on = drive_is_armed() && emulator_is_selected() &&
                  gpio_get_level(PIN_FDD_MOTOR) == 0;

        if (on != shown) {
            shown = on;
            if (on) {
                led_set(STATUS_LED_R, STATUS_LED_G, STATUS_LED_B);
            } else {
                led_set(0, 0, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(STATUS_POLL_MS));
    }
}

esp_err_t status_led_init(void)
{
    const rmt_tx_channel_config_t cfg = {
        .gpio_num = STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = FLUX_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&cfg, &led_chan);
    if (err != ESP_OK) {
        printf("Status LED: RMT channel failed (%s)\n", esp_err_to_name(err));
        return err;
    }

    const rmt_bytes_encoder_config_t enc = {
        .bit0 = { .level0 = 1, .duration0 = T0H, .level1 = 0, .duration1 = T0L },
        .bit1 = { .level0 = 1, .duration0 = T1H, .level1 = 0, .duration1 = T1L },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc, &led_enc));
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    led_set(0, 0, 0);
    xTaskCreate(status_led_task, "status_led", 3072, NULL, 2, NULL);
    printf("Status LED: GPIO%d, on while %s is selected and MOTOR is active\n",
           STATUS_LED_GPIO, EMU_SELECT_NAME);
    return ESP_OK;
}
