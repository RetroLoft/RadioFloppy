/*
 * Floppy stepper-motor sound. See step_sound.h.
 *
 * Buzzer: active 5 V buzzer on GPIO20 via a BC817 low-side switch, GPIO
 * HIGH = sound (as in button_test.c; needs +5V on J2).
 *
 * The drive ISR only switches the buzzer on and starts a one-shot
 * esp_timer (both IRAM, no waiting). The timer callback, in the esp_timer
 * task, switches it off, keeps it off for STEP_SOUND_MIN_OFF_MS and then
 * plays at most one click that was requested meanwhile. A STEP during a
 * click therefore never stretches it into a continuous tone. Missing a
 * click is fine; the floppy path never waits for the sound.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "esp_attr.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "step_sound.h"

typedef enum {
    SOUND_IDLE,
    SOUND_CLICK,        /* buzzer on */
    SOUND_GAP,          /* forced silence after a click */
} sound_state_t;

static portMUX_TYPE sound_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t sound_timer;
static sound_state_t state = SOUND_IDLE;
static bool pending;

/* Call with sound_lock held. */
static void IRAM_ATTR start_click_locked(void)
{
    gpio_ll_set_level(&GPIO, PIN_BUZZER, 1);
    state = SOUND_CLICK;
    esp_timer_start_once(sound_timer, STEP_SOUND_PULSE_MS * 1000);
}

static void sound_timer_cb(void *arg)
{
    portENTER_CRITICAL(&sound_lock);
    if (state == SOUND_CLICK) {
        gpio_ll_set_level(&GPIO, PIN_BUZZER, 0);
        state = SOUND_GAP;
        esp_timer_start_once(sound_timer, STEP_SOUND_MIN_OFF_MS * 1000);
    } else if (state == SOUND_GAP) {
        if (pending) {
            pending = false;
            start_click_locked();
        } else {
            state = SOUND_IDLE;
        }
    }
    portEXIT_CRITICAL(&sound_lock);
}

void step_sound_init(void)
{
    gpio_set_level(PIN_BUZZER, 0);
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BUZZER,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_BUZZER, 0);

    const esp_timer_create_args_t args = {
        .callback = sound_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "step_sound",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &sound_timer));

#if STEP_SOUND_ENABLED
    printf("RadioFloppy STEP sound enabled\n");
    printf("Buzzer: GPIO%d\n", PIN_BUZZER);
    printf("Pulse duration: %d ms (min. off %d ms)\n", STEP_SOUND_PULSE_MS,
           STEP_SOUND_MIN_OFF_MS);
#endif
}

void IRAM_ATTR step_sound_step_isr(void)
{
#if STEP_SOUND_ENABLED
    portENTER_CRITICAL_ISR(&sound_lock);
    if (state == SOUND_IDLE) {
        start_click_locked();
    } else {
        pending = true;     /* at most one click queued */
    }
    portEXIT_CRITICAL_ISR(&sound_lock);
#endif
}

void IRAM_ATTR step_sound_stop_isr(void)
{
    portENTER_CRITICAL_ISR(&sound_lock);
    if (state != SOUND_IDLE || pending) {
        esp_timer_stop(sound_timer);
        gpio_ll_set_level(&GPIO, PIN_BUZZER, 0);
        state = SOUND_IDLE;
        pending = false;
    }
    portEXIT_CRITICAL_ISR(&sound_lock);
}
