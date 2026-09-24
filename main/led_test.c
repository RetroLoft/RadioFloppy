/*
 * RadioFloppy - on-board RGB LED test.
 *
 * Blinks an addressable (WS2812-type) LED on two candidate pins, each with
 * its own RMT channel and colour, to find out where the LED is connected:
 *   GPIO38: blue,  GPIO48: red   (0.5 s on / 0.5 s off)
 * The floppy emulation does not run in this mode.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

#include "tests.h"

#define LED_RES_HZ  10000000    /* 0.1 us per tick */
#define BLINK_MS    500
#define LEVEL       64          /* brightness of the lit colour */

typedef struct {
    int gpio;
    const char *colour;
    uint8_t grb[3];
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
} test_led_t;

static test_led_t leds[] = {
    { .gpio = 38, .colour = "blue", .grb = { 0, 0, LEVEL } },
    { .gpio = 48, .colour = "red",  .grb = { 0, LEVEL, 0 } },
};

static void led_send(test_led_t *led, bool on)
{
    static const uint8_t off[3] = { 0, 0, 0 };
    const rmt_transmit_config_t tx = { .loop_count = 0 };

    rmt_transmit(led->chan, led->enc, on ? led->grb : off, 3, &tx);
    rmt_tx_wait_all_done(led->chan, pdMS_TO_TICKS(10));
}

void led_test_run(void)
{
    /* WS2812 bits as ESP-IDF's led_strip: 0 = 0.3/0.9 us, 1 = 0.9/0.3 us. */
    const rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };

    printf("\nRadioFloppy - RGB LED test\n");
    for (size_t i = 0; i < sizeof(leds) / sizeof(leds[0]); i++) {
        const rmt_tx_channel_config_t cfg = {
            .gpio_num = leds[i].gpio,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = LED_RES_HZ,
            .mem_block_symbols = 48,
            .trans_queue_depth = 1,
        };
        ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg, &leds[i].chan));
        ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &leds[i].enc));
        ESP_ERROR_CHECK(rmt_enable(leds[i].chan));
        printf("GPIO%d: blinking %s\n", leds[i].gpio, leds[i].colour);
    }

    bool on = false;
    while (true) {
        on = !on;
        for (size_t i = 0; i < sizeof(leds) / sizeof(leds[0]); i++) {
            led_send(&leds[i], on);
        }
        vTaskDelay(pdMS_TO_TICKS(BLINK_MS));
    }
}
