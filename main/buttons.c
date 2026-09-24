/*
 * Front panel buttons. See buttons.h.
 *
 * A low-priority task on core 1 polls both buttons every POLL_MS; a press
 * counts once the level has been stable LOW for DEBOUNCE_MS. The disk
 * switch runs in the same task, so presses during a switch (a few
 * seconds) are ignored rather than queued; the floppy interrupts are never
 * involved.
 */
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "board_pins.h"
#include "buttons.h"
#include "disk_switch.h"
#include "step_sound.h"

#define POLL_MS         5
#define DEBOUNCE_MS     30

typedef struct {
    gpio_num_t pin;
    int dir;                /* -1 left, +1 right */
    const char *name;
    int stable;             /* debounced level */
    int last_raw;
    TickType_t changed_at;
} button_t;

static button_t buttons[] = {
    { .pin = PIN_BTN_NEXT, .dir = -1, .name = "LEFT" },     /* GPIO19 */
    { .pin = PIN_BTN_PREV, .dir = +1, .name = "RIGHT" },    /* GPIO8 */
};

#define NUM_BUTTONS (sizeof(buttons) / sizeof(buttons[0]))

static void resync(void)
{
    TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        int level = gpio_get_level(buttons[i].pin);
        buttons[i].stable = level;
        buttons[i].last_raw = level;
        buttons[i].changed_at = now;
    }
}

static void do_switch(const button_t *b)
{
    switch_error_t e;
    disk_info_t d;

    step_sound_click();         /* immediate feedback */
    printf("Button %s: %s disk...\n", b->name, b->dir < 0 ? "previous" : "next");
    if (disk_switch_step(b->dir, &e) == ESP_OK) {
        disk_get_current(&d);
        if (d.source == DISK_SRC_PSRAM) {
            printf("Button %s: disk 0 (PSRAM) \"%s\"\n", b->name, d.name);
        } else {
            printf("Button %s: slot %d \"%s\"\n", b->name, d.slot + 1, d.name);
        }
    } else {
        printf("Button %s: not changed - %s (%s)\n", b->name, e.msg, e.code);
    }
}

static void buttons_task(void *arg)
{
    resync();
    while (true) {
        TickType_t now = xTaskGetTickCount();
        for (size_t i = 0; i < NUM_BUTTONS; i++) {
            button_t *b = &buttons[i];
            int raw = gpio_get_level(b->pin);

            if (raw != b->last_raw) {
                b->last_raw = raw;
                b->changed_at = now;
            } else if (raw != b->stable &&
                       (now - b->changed_at) >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
                b->stable = raw;
                if (raw == 0) {             /* active low: pressed */
                    do_switch(b);
                    resync();               /* presses during the switch are dropped */
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_PREV) | (1ULL << PIN_BTN_NEXT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,       /* buttons switch to GND */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    xTaskCreatePinnedToCore(buttons_task, "buttons", 4096, NULL, 3, NULL, 1);
    printf("Buttons: LEFT (GPIO%d) = previous disk, RIGHT (GPIO%d) = next disk\n",
           PIN_BTN_NEXT, PIN_BTN_PREV);
}
