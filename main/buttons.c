/*
 * Front panel buttons. See buttons.h.
 *
 * A low-priority task on core 1 polls both buttons every POLL_MS; a level
 * counts once it has been stable for DEBOUNCE_MS. A press clicks at once;
 * the disk changes when a single button is released. Holding both buttons
 * for CHORD_MS shows the network name and IP address on the OLED and never
 * changes the disk; holding them on to SETUP_MS restarts into WiFi setup
 * mode. In setup mode single presses do nothing and CHORD_MS leaves it. The disk switch runs in this task, so presses during a
 * switch are ignored rather than queued; the floppy interrupts are never
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
#include "oled.h"
#include "setup_mode.h"
#include "step_sound.h"

#define POLL_MS         5
#define DEBOUNCE_MS     30
#define CHORD_MS        3000    /* both buttons: show network info */
#define SETUP_MS        10000   /* both buttons: WiFi setup mode */

static void (*setup_exit)(void);    /* set: setup mode */

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

    printf("Button %s: %s disk...\n", b->name, b->dir < 0 ? "previous" : "next");
    if (disk_switch_step(b->dir, &e) == ESP_OK) {
        disk_get_current(&d);
        if (d.source == DISK_SRC_PSRAM) {
            printf("Button %s: disk 0 (PSRAM) \"%s\"\n", b->name, d.name);
        } else {
            printf("Button %s: image %u \"%s\"\n", b->name, d.image_id, d.name);
        }
    } else {
        printf("Button %s: not changed - %s (%s)\n", b->name, e.msg, e.code);
    }
}

static void buttons_task(void *arg)
{
    bool both_seen = false;         /* both were down in this press session */
    bool chord_done = false;
    bool setup_done = false;
    bool armed = !setup_exit;       /* setup mode: wait for a release first */
    TickType_t both_since = 0;

    resync();
    while (true) {
        TickType_t now = xTaskGetTickCount();
        const button_t *released = NULL;

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
                    step_sound_click();     /* immediate feedback */
                } else {
                    released = b;
                }
            }
        }

        bool down0 = buttons[0].stable == 0, down1 = buttons[1].stable == 0;
        if (down0 && down1) {
            if (!both_seen) {
                both_seen = true;
                both_since = now;
            } else if (!chord_done && armed && now - both_since >= pdMS_TO_TICKS(CHORD_MS)) {
                chord_done = true;
                if (setup_exit) {
                    setup_exit();
                } else {
                    printf("Buttons: both held %d s - showing network info\n", CHORD_MS / 1000);
                    oled_show_network();
                }
            } else if (!setup_done && !setup_exit &&
                       now - both_since >= pdMS_TO_TICKS(SETUP_MS)) {
                setup_done = true;
                printf("Buttons: both held %d s - WiFi setup mode\n", SETUP_MS / 1000);
                oled_show_message("WiFi setup mode", "Restarting...");
                vTaskDelay(pdMS_TO_TICKS(1500));
                setup_mode_restart(SETUP_BUTTONS);
            }
        }

        if (released && !both_seen && !setup_exit) {
            do_switch(released);            /* short press of one button */
            resync();                       /* presses during the switch are dropped */
        }
        if (!down0 && !down1) {
            both_seen = false;              /* session over */
            chord_done = false;
            armed = true;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static void buttons_config(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BTN_PREV) | (1ULL << PIN_BTN_NEXT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,       /* buttons switch to GND */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

void buttons_start(void)
{
    buttons_config();
    xTaskCreatePinnedToCore(buttons_task, "buttons", 4096, NULL, 3, NULL, 1);
    printf("Buttons: LEFT (GPIO%d) = previous disk, RIGHT (GPIO%d) = next disk, "
           "both %d s = network info, both %d s = WiFi setup\n", PIN_BTN_NEXT, PIN_BTN_PREV,
           CHORD_MS / 1000, SETUP_MS / 1000);
}

void buttons_start_setup(void (*on_exit)(void))
{
    setup_exit = on_exit;
    buttons_config();
    xTaskCreatePinnedToCore(buttons_task, "buttons", 4096, NULL, 3, NULL, 1);
}
