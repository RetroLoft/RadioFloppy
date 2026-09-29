/*
 * The LEDs on J4. See leds.h.
 *
 * A low-priority task polls the drive and WiFi state every POLL_MS and
 * only writes a GPIO when its level changes; the floppy interrupts and the
 * flux stream are never involved.
 */
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "leds.h"
#include "wifi_net.h"

#define POLL_MS         20
#define SLOW_BLINK_MS   500     /* joining */
#define FAST_BLINK_MS   125     /* setup mode */

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

static void leds_task(void *arg)
{
    int activity = -1, status = -1;

    while (true) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        int a = drive_is_armed() && emulator_is_selected() && gpio_get_level(PIN_FDD_MOTOR) == 0;
        int s = status_level(now_ms);

        if (a != activity) {
            activity = a;
            gpio_set_level(PIN_LED_ACTIVITY, a);
        }
        if (s != status) {
            status = s;
            gpio_set_level(PIN_LED_STATUS, s);
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
    gpio_set_level(PIN_LED_ACTIVITY, 0);
    gpio_set_level(PIN_LED_STATUS, 0);
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LED_ACTIVITY) | (1ULL << PIN_LED_STATUS),
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
    printf("LEDs: activity GPIO%d (%s selected + MOTOR), status GPIO%d (WiFi)\n",
           PIN_LED_ACTIVITY, EMU_SELECT_NAME, PIN_LED_STATUS);
    return ESP_OK;
}
