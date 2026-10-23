/*
 * The front panel LEDs. See leds.h.
 *
 * A low-priority task polls the drive and WiFi state every POLL_MS and
 * only writes an LED when its state changes; the floppy interrupts and
 * the flux stream are never involved. The on-board RGB LED (WS2812, GRB)
 * gets its own RMT TX channel without DMA, created only while it is used;
 * it shares the 10 MHz RMT group clock with the flux channels.
 */
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "flux_stream.h"
#include "leds.h"
#include "settings.h"
#include "wifi_net.h"

#define POLL_MS         20
#define SLOW_BLINK_MS   500     /* joining */
#define FAST_BLINK_MS   125     /* setup mode */
#define SETTINGS_MS     250     /* how often the "leds" setting is looked at */
#define RGB_BLUE        24      /* on-board LED brightness, of 255 */

/* WS2812 bit timing at 10 MHz (0.1 us per tick): 0 = 0.3/0.9 us, 1 = 0.9/0.3 us. */
#define T0H 3
#define T0L 9
#define T1H 9
#define T1L 3

static bool blink(int64_t now_ms, int half_period_ms)
{
    return (now_ms / half_period_ms) % 2 == 0;
}

static bool status_level(int64_t now_ms)
{
    wifi_net_status_t w;

    wifi_net_get_status(&w);
    if (w.ap_mode) {
        return blink(now_ms, FAST_BLINK_MS);
    }
    if (w.connected) {
        return false;
    }
    if (!w.configured || (w.started && w.down_ms >= LEDS_WIFI_GIVE_UP_MS)) {
        return true;
    }
    return blink(now_ms, SLOW_BLINK_MS);           /* starting or joining */
}

/* ---- On-board RGB LED (1-LED mode) --------------------------------------------- */

static rmt_channel_handle_t rgb_chan;
static rmt_encoder_handle_t rgb_enc;

static bool rgb_set(bool on)
{
    const uint8_t grb[3] = { 0, 0, on ? RGB_BLUE : 0 };
    const rmt_transmit_config_t tx = { .loop_count = 0 };

    /* The idle LOW until the next update (>= POLL_MS) is the latch/reset. */
    return rmt_transmit(rgb_chan, rgb_enc, grb, sizeof(grb), &tx) == ESP_OK &&
           rmt_tx_wait_all_done(rgb_chan, pdMS_TO_TICKS(10)) == ESP_OK;
}

static bool rgb_start(void)
{
    const rmt_tx_channel_config_t cfg = {
        .gpio_num = PIN_LED_J4_PIN1,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = FLUX_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    if (!rgb_enc) {
        const rmt_bytes_encoder_config_t enc = {
            .bit0 = { .level0 = 1, .duration0 = T0H, .level1 = 0, .duration1 = T0L },
            .bit1 = { .level0 = 1, .duration0 = T1H, .level1 = 0, .duration1 = T1L },
            .flags.msb_first = 1,
        };
        if (rmt_new_bytes_encoder(&enc, &rgb_enc) != ESP_OK) {
            return false;
        }
    }
    if (rmt_new_tx_channel(&cfg, &rgb_chan) != ESP_OK) {
        rgb_chan = NULL;
        return false;
    }
    if (rmt_enable(rgb_chan) != ESP_OK) {
        rmt_del_channel(rgb_chan);
        rgb_chan = NULL;
        return false;
    }
    return true;
}

static void rgb_stop(void)
{
    if (rgb_chan) {
        rgb_set(false);
        rmt_disable(rgb_chan);
        rmt_del_channel(rgb_chan);
        rgb_chan = NULL;
    }
}

/* ---- Task ---------------------------------------------------------------------- */

/* GPIO48: plain output for an LED on J4 pin 1 (2 LEDs) or the RGB LED (1 LED). */
static void use_mode(int leds)
{
    if (leds == 2) {
        rgb_stop();
        gpio_reset_pin(PIN_LED_J4_PIN1);
        gpio_set_direction(PIN_LED_J4_PIN1, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_LED_J4_PIN1, 0);
    } else if (!rgb_chan && !rgb_start()) {
        printf("LEDs: on-board RGB LED not available (no RMT channel)\n");
    }
    printf("LEDs: %d fitted - activity on J4 pin 3, WiFi status %s\n", leds,
           leds == 2 ? "on J4 pin 1" : "on the on-board RGB LED");
}

static void leds_task(void *arg)
{
    int activity = -1, wifi = -1, mode = 0;
    int64_t settings_at = 0;

    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - settings_at >= SETTINGS_MS || !mode) {
            settings_t cfg;
            settings_get(&cfg);
            settings_at = now_ms;
            if (cfg.leds != mode) {
                mode = cfg.leds == 2 ? 2 : 1;
                use_mode(mode);
                wifi = -1;              /* show it again on the new LED */
            }
        }
        int a = drive_is_armed() && emulator_is_selected() && gpio_get_level(PIN_FDD_MOTOR) == 0;
        int w = status_level(now_ms);

        if (a != activity) {
            activity = a;
            gpio_set_level(PIN_LED_J4_PIN3, a);
        }
        if (w != wifi) {
            if (mode == 2) {
                gpio_set_level(PIN_LED_J4_PIN1, w);
                wifi = w;
            } else if (rgb_chan && rgb_set(w)) {
                wifi = w;               /* else: try again next time */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t leds_init(void)
{
    static bool started;

    if (started) {
        return ESP_OK;
    }
    gpio_set_level(PIN_LED_J4_PIN3, 0);
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LED_J4_PIN3,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        printf("LEDs: %s\n", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(leds_task, "leds", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    started = true;
    return ESP_OK;
}
