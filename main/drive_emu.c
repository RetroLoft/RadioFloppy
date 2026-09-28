/*
 * Virtual Shugart drive. See drive_emu.h.
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

#include "soc/io_mux_reg.h"

#include "board_pins.h"
#include "disk_image.h"
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
gpio_num_t emu_select_line = EMU_DS1;
static volatile uint32_t ignored_steps;
static bool disk_present;
static int64_t media_change_until_us;
static volatile bool writable;          /* WPROT released: see drive_set_writable() */
static volatile int64_t last_index_us;  /* start of the last INDEX pulse */
static drive_write_start_t write_start; /* last WGATE assertion */
static bool wrote_while_selected;       /* WGATE since our drive was selected */

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
    /* Write protect: asserted for a read-only disk; released for a disk
     * that may be written. During a disk change the opposite, so TOS always
     * sees the transition it uses to notice the change. */
    st->wprot = active && (changing ? writable : !writable);
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
    if (ev.type == DRV_EV_WGATE && ev.st.wgate && armed && ev.st.selected) {
        /* A write starts: where on which track, and was it allowed. */
        write_start = (drive_write_start_t) {
            .time_us = esp_timer_get_time(),
            .index_us = last_index_us,
            .cyl = current_track,
            .side = ev.st.side,
            .writable = writable && !ev.st.wprot,
            .gen = disk_media_gen,
            .seq = write_start.seq + 1,
        };
        wrote_while_selected = true;
    }
    if (ev.type == DRV_EV_SELECT && !ev.st.selected) {
        wrote_while_selected = false;
    }
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

/* Our own INDEX output (read back from the pad): the rotation reference. */
static void IRAM_ATTR index_isr(void *arg)
{
    last_index_us = esp_timer_get_time();
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

    const struct {                  /* select line: from the settings */
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

    /* INDEX is an output (RMT, GPIO HIGH = pulse); its pad is read back
     * so the rising edge marks the start of every revolution. */
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_FDD_INDEX]);
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_FDD_INDEX, GPIO_INTR_POSEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_FDD_INDEX, index_isr, NULL));
}

void drive_arm(drive_status_t *status)
{
    portENTER_CRITICAL(&drive_lock);
    armed = true;
    update_outputs_locked(status);
    portEXIT_CRITICAL(&drive_lock);
}

void drive_peek(drive_status_t *st)
{
    *st = (drive_status_t) {
        .armed = armed,
        .selected = emulator_is_selected(),
        .motor = gpio_ll_get_level(&GPIO, PIN_FDD_MOTOR) == 0,
        .wgate = gpio_ll_get_level(&GPIO, PIN_FDD_WGATE) == 0,
        .cyl = current_track,
        .side = gpio_ll_get_level(&GPIO, PIN_FDD_SIDE) ? 0 : 1,
    };
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

drive_swap_t drive_swap_media(void (*swap)(void *), void *arg, bool present, bool allow_selected)
{
    drive_status_t st;
    drive_swap_t done = DRIVE_SWAP_REFUSED;

    portENTER_CRITICAL(&drive_lock);
    if (!(armed && emulator_is_selected())) {
        done = DRIVE_SWAP_IDLE;
    } else if (allow_selected && (!writable || (!wrote_while_selected &&
                                                gpio_ll_get_level(&GPIO, PIN_FDD_WGATE) != 0))) {
        done = DRIVE_SWAP_SELECTED;
    }
    if (done != DRIVE_SWAP_REFUSED) {
        swap(arg);
        disk_present = present;
        media_change_until_us = esp_timer_get_time() + DRIVE_MEDIA_CHANGE_MS * 1000LL;
        wrote_while_selected = false;   /* that was the old disk */
        update_outputs_locked(&st);
    }
    portEXIT_CRITICAL(&drive_lock);
    return done;
}

void drive_set_writable(bool on)
{
    drive_status_t st;

    portENTER_CRITICAL(&drive_lock);
    writable = on;
    update_outputs_locked(&st);
    portEXIT_CRITICAL(&drive_lock);
}

bool drive_writable(void)
{
    return writable;
}

void drive_get_write_start(drive_write_start_t *ws)
{
    portENTER_CRITICAL(&drive_lock);
    *ws = write_start;
    portEXIT_CRITICAL(&drive_lock);
}

bool drive_is_armed(void)
{
    return armed;
}

uint32_t drive_ignored_steps(void)
{
    return ignored_steps;
}
