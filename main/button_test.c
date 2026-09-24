/*
 * FloppyEmulator-ESP32S3 - push button + buzzer test.
 *
 * Prints a message on the UART0 console for every press of the left
 * (SW_NEXT, GPIO19) or right (SW_PREV, GPIO8) button. Buttons pull the
 * GPIO to GND; internal pull-ups are used. Software debounce: a new level
 * is only accepted after it has been stable for DEBOUNCE_MS.
 *
 * Phase 2: the active 5 V buzzer (GPIO20 -> BC817 low-side switch,
 * HIGH = on) beeps once for BEEP_STARTUP_MS after start-up.
 */
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "board_pins.h"
#include "tests.h"

#define POLL_INTERVAL_MS    5
#define DEBOUNCE_MS         30
#define BEEP_STARTUP_MS     500

typedef struct {
    gpio_num_t pin;
    const char *name;
    int stable_level;       /* debounced level */
    int last_raw_level;     /* level seen at previous poll */
    TickType_t changed_at;  /* tick of the last raw level change */
} button_t;

static button_t buttons[] = {
    { .pin = PIN_BTN_NEXT, .name = "Left Button"  },
    { .pin = PIN_BTN_PREV, .name = "Right Button" },
};

#define NUM_BUTTONS (sizeof(buttons) / sizeof(buttons[0]))

static void buzzer_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BUZZER,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* Set the output latch low before enabling the driver so the buzzer
     * stays silent. For GPIO20 this also detaches the native USB D+ pad. */
    gpio_set_level(PIN_BUZZER, 0);
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void buzzer_beep(uint32_t duration_ms)
{
    gpio_set_level(PIN_BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(PIN_BUZZER, 0);
}

static void buttons_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_PREV) | (1ULL << PIN_BTN_NEXT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* For GPIO19 this also detaches the pad from the native USB PHY. */
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* Let the pull-ups settle, then take the current level as the start
     * state so a button held during reset does not produce a message. */
    vTaskDelay(pdMS_TO_TICKS(10));
    TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        int level = gpio_get_level(buttons[i].pin);
        buttons[i].stable_level = level;
        buttons[i].last_raw_level = level;
        buttons[i].changed_at = now;
    }
}

static void buttons_poll(void)
{
    TickType_t now = xTaskGetTickCount();

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        button_t *b = &buttons[i];
        int raw = gpio_get_level(b->pin);

        if (raw != b->last_raw_level) {
            b->last_raw_level = raw;
            b->changed_at = now;
        } else if (raw != b->stable_level &&
                   (now - b->changed_at) >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
            b->stable_level = raw;
            if (raw == 0) {          /* active low: 0 = pressed */
                printf("%s\n", b->name);
            }
        }
    }
}

void button_test_run(void)
{
    printf("\nFloppy Emulator - Button Test\n");

    buzzer_init();
    buttons_init();

    printf("Ready. Press a button.\n");

    buzzer_beep(BEEP_STARTUP_MS);

    while (true) {
        buttons_poll();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
