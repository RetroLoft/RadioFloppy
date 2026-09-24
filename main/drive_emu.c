/*
 * Virtual read-only Shugart drive. See drive_emu.h.
 *
 * Every interrupt recomputes all outputs from the live select level under
 * a spinlock. Whichever of a STEP, MOTOR, WGATE and deselect interrupt runs
 * last sees the real select level, so no output can stay active after
 * deselection. STEP moves the head on the trailing (rising) edge of the
 * active-low pulse.
 */
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "esp_attr.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "flux_stream.h"
#include "step_sound.h"

#define EVENT_QUEUE_LEN 256

static portMUX_TYPE drive_lock = portMUX_INITIALIZER_UNLOCKED;
static bool armed;
static volatile int current_track = 0;  /* assumed at track 0 at start-up */
static QueueHandle_t event_queue;
static volatile uint32_t select_edges;
static volatile uint32_t ignored_steps;
static bool disk_present;
static int64_t media_change_until_us;

int IRAM_ATTR drive_cylinder(void)
{
    return current_track;
}

/* Call with drive_lock held. IRAM-safe. */
static void IRAM_ATTR update_outputs_locked(drive_status_t *st)
{
    bool active = armed && emulator_is_selected();
    bool changing = media_change_until_us && esp_timer_get_time() < media_change_until_us;
    bool disk = disk_present && !changing;

    st->armed = armed;
    st->selected = emulator_is_selected();
    st->motor = gpio_ll_get_level(&GPIO, PIN_FDD_MOTOR) == 0;
    st->wgate = gpio_ll_get_level(&GPIO, PIN_FDD_WGATE) == 0;
    st->track0 = active && current_track == 0;
    st->wprot = active && !changing;
    st->index = active && disk && st->motor;
    st->rdata = active && disk && st->motor && !st->wgate;
    st->cyl = current_track;
    st->side = gpio_ll_get_level(&GPIO, PIN_FDD_SIDE) ? 0 : 1;

    /* GPIO HIGH = ULN2003A pulls the Shugart line LOW = asserted. */
    gpio_ll_set_level(&GPIO, PIN_FDD_TRK0, st->track0);
    gpio_ll_set_level(&GPIO, PIN_FDD_WPROT, st->wprot);
    flux_gate(st->rdata, st->index);

    if (!active) {
        step_sound_stop_isr();      /* no sound from us for another drive */
    }
}

static void IRAM_ATTR drive_isr(void *arg)
{
    drive_event_t ev = { .type = (uint8_t)(uintptr_t)arg };
    bool log;

    portENTER_CRITICAL_ISR(&drive_lock);
    if (ev.type == DRV_EV_STEP && armed && emulator_is_selected()) {
        if (gpio_ll_get_level(&GPIO, PIN_FDD_DIR) == 0) {
            if (current_track < DRIVE_MAX_TRACK) {
                current_track++;        /* DIR LOW: inward */
            }
        } else if (current_track > 0) {
            current_track--;            /* DIR HIGH: towards track 0 */
        }
        step_sound_step_isr();          /* also at a limit: the motor still clicks */
        log = true;
    } else {
        log = ev.type == DRV_EV_SELECT ? armed : armed && emulator_is_selected();
        if (ev.type == DRV_EV_STEP) {
            ignored_steps++;
            log = false;
        }
    }
    update_outputs_locked(&ev.st);
    portEXIT_CRITICAL_ISR(&drive_lock);

    if (ev.type == DRV_EV_SELECT) {
        select_edges++;
    }
    if (!log) {
        return;
    }

    ev.time_us = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(event_queue, &ev, &woken);   /* full: drop log only */
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

void drive_init(void)
{
    event_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(drive_event_t));
    assert(event_queue);

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_FDD_DS0) | (1ULL << PIN_FDD_DS1) |
                        (1ULL << PIN_FDD_MOTOR) | (1ULL << PIN_FDD_DIR) |
                        (1ULL << PIN_FDD_STEP) | (1ULL << PIN_FDD_WDATA) |
                        (1ULL << PIN_FDD_WGATE) | (1ULL << PIN_FDD_SIDE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,      /* driven by the SN74LVC245A */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    static const struct {
        gpio_num_t pin;
        gpio_int_type_t edge;
        drive_event_type_t type;
    } irqs[] = {
        { EMU_SELECT_LINE, GPIO_INTR_ANYEDGE, DRV_EV_SELECT },
        { PIN_FDD_MOTOR,   GPIO_INTR_ANYEDGE, DRV_EV_MOTOR },
        { PIN_FDD_WGATE,   GPIO_INTR_ANYEDGE, DRV_EV_WGATE },
        { PIN_FDD_SIDE,    GPIO_INTR_ANYEDGE, DRV_EV_SIDE },
        { PIN_FDD_STEP,    GPIO_INTR_POSEDGE, DRV_EV_STEP },   /* trailing edge */
    };

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    for (size_t i = 0; i < sizeof(irqs) / sizeof(irqs[0]); i++) {
        ESP_ERROR_CHECK(gpio_set_intr_type(irqs[i].pin, irqs[i].edge));
        ESP_ERROR_CHECK(gpio_isr_handler_add(irqs[i].pin, drive_isr,
                                             (void *)(uintptr_t)irqs[i].type));
    }
}

void drive_arm(drive_status_t *status)
{
    portENTER_CRITICAL(&drive_lock);
    armed = true;
    update_outputs_locked(status);
    portEXIT_CRITICAL(&drive_lock);
}

void drive_refresh(drive_status_t *status)
{
    portENTER_CRITICAL(&drive_lock);
    update_outputs_locked(status);
    portEXIT_CRITICAL(&drive_lock);
}

QueueHandle_t drive_events(void)
{
    return event_queue;
}

uint32_t drive_select_edges(void)
{
    return select_edges;
}

bool drive_swap_media(void (*swap)(void *), void *arg, bool present)
{
    drive_status_t st;
    bool done = false;

    portENTER_CRITICAL(&drive_lock);
    if (!(armed && emulator_is_selected())) {
        swap(arg);
        disk_present = present;
        media_change_until_us = esp_timer_get_time() + DRIVE_MEDIA_CHANGE_MS * 1000LL;
        update_outputs_locked(&st);
        done = true;
    }
    portEXIT_CRITICAL(&drive_lock);
    return done;
}

bool drive_is_armed(void)
{
    return armed;
}

uint32_t drive_ignored_steps(void)
{
    return ignored_steps;
}
