/*
 * FloppyEmulator-ESP32S3 - selection-aware Atari Shugart monitor with a
 * minimal virtual drive (head position + TRACK0 output, see drive_emu.c).
 *
 * Only TRACK0 (GPIO2) is ever activated, by drive_emu.c from ISR context.
 * The other five Shugart outputs stay released (GPIO LOW) the whole time.
 *
 * Start-up guard: the drive is only armed after the selected drive-select
 * line has been HIGH for ARM_HIGH_MS without interruption (an unpowered
 * Atari reads as "selected"). Once armed, it stays armed.
 *
 * MOTOR, DIR, STEP, SIDE, WGATE and WDATA are shared by all drives on the
 * cable, so they are only reported while our drive is selected
 * (emulator_is_selected(), see drive_config.h).
 *
 *  - Select line, MOTOR, DIR, SIDE, WGATE: GPIO interrupt on both edges.
 *  - STEP: GPIO interrupt on the falling edge (start of the active-low
 *    pulse).
 *  - WDATA: counted in hardware by a PCNT unit, gated by WGATE.
 *
 * Each ISR takes a snapshot of all input levels (one register read), the
 * WDATA count and a timestamp, and queues it; shared-signal events are only
 * queued while our drive is selected. All printing happens in task context.
 * Levels are reported raw; ACTIVE/INACTIVE etc. are Shugart-convention
 * labels only.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"

#include "board_pins.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "tests.h"

#define EVENT_QUEUE_LEN     1024
#define RESYNC_INTERVAL_MS  100   /* idle re-check of the levels */
#define STEP_LINES_MAX      16    /* individual STEP lines per selection */
#define STEP_SUMMARY_MS     250   /* then one summary line per burst/period */
#define PCNT_HIGH_LIMIT     32767
#define ARM_HIGH_MS         50    /* start-up guard, select line HIGH */
#define IGNORED_REPORT_MS   500   /* quiet time before reporting ignored STEPs */

/* All Shugart inputs are GPIO0..31, so one read of GPIO_IN_REG is a
 * consistent snapshot of every input. */
_Static_assert(PIN_FDD_DS0 < 32 && PIN_FDD_DS1 < 32 && PIN_FDD_MOTOR < 32 &&
               PIN_FDD_DIR < 32 && PIN_FDD_STEP < 32 && PIN_FDD_WDATA < 32 &&
               PIN_FDD_WGATE < 32 && PIN_FDD_SIDE < 32,
               "input snapshot assumes GPIO0..31");

typedef enum {
    EV_SELECT,
    EV_MOTOR,
    EV_DIR,
    EV_SIDE,
    EV_WGATE,
    EV_STEP,
} event_type_t;

typedef struct {
    int64_t time_us;
    uint32_t inputs;    /* GPIO_IN_REG snapshot */
    int wdata_count;    /* PCNT snapshot */
    drive_status_t drive;   /* after the ISR update (EV_SELECT, EV_STEP) */
    int track_before;       /* EV_STEP */
    uint8_t type;
} monitor_event_t;

typedef struct {
    const char *name;
    gpio_num_t gpio;
    const char *low_text;   /* meaning of LOW per Shugart convention */
    const char *high_text;
} signal_t;

/* Level signals with an edge interrupt, indexed by event_type_t. */
static const signal_t signals[] = {
    [EV_SELECT] = { NULL,    EMU_SELECT_LINE, "selected", "not selected" },
    [EV_MOTOR]  = { "MOTOR", PIN_FDD_MOTOR,   "ACTIVE",   "INACTIVE" },
    [EV_DIR]    = { "DIR",   PIN_FDD_DIR,     "IN, to higher tracks", "OUT, to track 0" },
    [EV_SIDE]   = { "SIDE",  PIN_FDD_SIDE,    "side 1",   "side 0" },
    [EV_WGATE]  = { "WGATE", PIN_FDD_WGATE,   "ACTIVE",   "INACTIVE" },
};

/* Order used for the snapshots. */
static const signal_t snapshot_signals[] = {
    { "DS0",   PIN_FDD_DS0,   "ACTIVE", "INACTIVE" },
    { "DS1",   PIN_FDD_DS1,   "ACTIVE", "INACTIVE" },
    { "MOTOR", PIN_FDD_MOTOR, "ACTIVE", "INACTIVE" },
    { "DIR",   PIN_FDD_DIR,   "IN, to higher tracks", "OUT, to track 0" },
    { "STEP",  PIN_FDD_STEP,  "ACTIVE", "INACTIVE" },
    { "SIDE",  PIN_FDD_SIDE,  "side 1", "side 0" },
    { "WGATE", PIN_FDD_WGATE, "ACTIVE", "INACTIVE" },
    { "WDATA", PIN_FDD_WDATA, "ACTIVE", "INACTIVE" },
};

#define NUM_SNAPSHOT_SIGNALS (sizeof(snapshot_signals) / sizeof(snapshot_signals[0]))

static QueueHandle_t event_queue;
static pcnt_unit_handle_t wdata_unit;
static volatile uint32_t dropped_events;
static volatile uint32_t select_edges;          /* for the start-up guard */
static volatile uint32_t ignored_steps;         /* not armed or not selected */
static volatile int64_t ignored_step_time_us;
static volatile bool monitor_armed;             /* events are only queued when armed */

/* ---- Interrupt side -------------------------------------------------- */

static void IRAM_ATTR signal_isr(void *arg)
{
    event_type_t type = (event_type_t)(uintptr_t)arg;
    monitor_event_t ev = { .type = type };

    /* Time-critical part first: outputs are updated before any logging. */
    if (type == EV_SELECT) {
        select_edges++;
        drive_select_changed_isr(&ev.drive);
        if (!monitor_armed) {
            return;
        }
    } else if (type == EV_STEP) {
        if (!drive_step_isr(&ev.drive, &ev.track_before)) {
            ignored_steps++;
            ignored_step_time_us = esp_timer_get_time();
            return;
        }
    } else if (!monitor_armed || !emulator_is_selected()) {
        /* Shared floppy signals only matter while our drive is selected. */
        return;
    }

    ev.time_us = esp_timer_get_time();
    ev.inputs = REG_READ(GPIO_IN_REG);
    if (type == EV_SELECT || type == EV_WGATE) {
        pcnt_unit_get_count(wdata_unit, &ev.wdata_count);
    }

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(event_queue, &ev, &woken) != pdTRUE) {
        dropped_events++;
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/* ---- Set-up ---------------------------------------------------------- */

static void inputs_init(void)
{
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
}

static void wdata_counter_init(void)
{
    const pcnt_unit_config_t unit_cfg = {
        .low_limit = -1,
        .high_limit = PCNT_HIGH_LIMIT,
        .flags.accum_count = 1,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &wdata_unit));

    const pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = PIN_FDD_WDATA,
        .level_gpio_num = PIN_FDD_WGATE,
    };
    pcnt_channel_handle_t chan;
    ESP_ERROR_CHECK(pcnt_new_channel(wdata_unit, &chan_cfg, &chan));

    /* Count falling edges of WDATA (start of each active-low pulse)... */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan,
                    PCNT_CHANNEL_EDGE_ACTION_HOLD,         /* rising  */
                    PCNT_CHANNEL_EDGE_ACTION_INCREASE));   /* falling */
    /* ...but only while WGATE is LOW. */
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan,
                    PCNT_CHANNEL_LEVEL_ACTION_HOLD,        /* WGATE high */
                    PCNT_CHANNEL_LEVEL_ACTION_KEEP));      /* WGATE low  */

    /* Needed for accum_count: the unit accumulates on each overflow. */
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(wdata_unit, PCNT_HIGH_LIMIT));

    ESP_ERROR_CHECK(pcnt_unit_enable(wdata_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(wdata_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(wdata_unit));
}

static void interrupts_init(void)
{
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));

    for (int t = EV_SELECT; t <= EV_WGATE; t++) {
        ESP_ERROR_CHECK(gpio_set_intr_type(signals[t].gpio, GPIO_INTR_ANYEDGE));
        ESP_ERROR_CHECK(gpio_isr_handler_add(signals[t].gpio, signal_isr,
                                             (void *)(uintptr_t)t));
    }

    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_FDD_STEP, GPIO_INTR_NEGEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_FDD_STEP, signal_isr,
                                         (void *)(uintptr_t)EV_STEP));
}

/* Check the pin configuration in the hardware registers. */
static void verify_pins(void)
{
    static const gpio_num_t outputs[] = {
        PIN_FDD_INDEX, PIN_FDD_TRK0, PIN_FDD_WPROT,
        PIN_FDD_RDATA, PIN_FDD_READY, PIN_FDD_DSKCHG,
    };
    uint64_t out_level = REG_READ(GPIO_OUT_REG) |
                         ((uint64_t)REG_READ(GPIO_OUT1_REG) << 32);
    uint64_t out_enable = REG_READ(GPIO_ENABLE_REG) |
                          ((uint64_t)REG_READ(GPIO_ENABLE1_REG) << 32);
    bool outputs_ok = true;

    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        if (!(out_enable & (1ULL << outputs[i])) || (out_level & (1ULL << outputs[i]))) {
            printf("ERROR: GPIO%d is not an output driving LOW!\n", outputs[i]);
            outputs_ok = false;
        }
    }
    if (!outputs_ok) {
        shugart_outputs_release();
    }
    printf("Outputs safely released%s (GPIO1, 2, 39, 40, 41, 42 = output LOW).\n",
           outputs_ok ? " and verified" : " (re-applied)");

    bool in_enabled = REG_GET_BIT(GPIO_PIN_MUX_REG[EMU_SELECT_LINE], FUN_IE);
    bool out_disabled = !(out_enable & (1ULL << EMU_SELECT_LINE));
    printf("Drive select line: %s / GPIO%d (EMU_SELECT_LINE in drive_config.h) - %s\n",
           EMU_SELECT_NAME, EMU_SELECT_LINE,
           in_enabled && out_disabled ? "input OK" : "ERROR: not configured as input!");
}

/* ---- Task side ------------------------------------------------------- */

static int level_of(uint32_t inputs, gpio_num_t gpio)
{
    return (inputs >> gpio) & 1;
}

static void print_timestamp(int64_t time_us)
{
    printf("[%6lld.%03lld] ", time_us / 1000000, (time_us / 1000) % 1000);
}

static void print_level(int64_t time_us, const signal_t *s, int level, const char *suffix)
{
    print_timestamp(time_us);
    printf("%-5s = %-4s (%s)%s\n", s->name, level ? "HIGH" : "LOW",
           level ? s->high_text : s->low_text, suffix);
}

static void print_snapshot(int64_t time_us, uint32_t inputs)
{
    for (size_t i = 0; i < NUM_SNAPSHOT_SIGNALS; i++) {
        print_level(time_us, &snapshot_signals[i],
                    level_of(inputs, snapshot_signals[i].gpio), "");
    }
}

/* State of the current selection period plus what has been logged. */
typedef struct {
    bool selected;
    int64_t start_us;
    int last_level[EV_WGATE + 1];
    uint32_t steps, steps_in, steps_out;
    uint32_t side_changes;
    uint32_t wgate_periods;
    bool wgate_active;
    int wgate_start_count;
    int wdata_pulses;
    /* STEP pulses collected for a summary line instead of single lines. */
    uint32_t pending_steps, pending_first;
    int pending_dir;
    int pending_track;          /* virtual track after the last pending pulse */
    int64_t pending_time_us;
    /* Last logged virtual drive state. */
    bool logged_track0;
    int logged_track;
} selection_t;

static void flush_pending_steps(selection_t *sel)
{
    if (sel->pending_steps == 0) {
        return;
    }
    print_timestamp(sel->pending_time_us);
    printf("STEP #%lu..#%lu %s (%lu pulses) -> virtual track %d\n",
           (unsigned long)sel->pending_first,
           (unsigned long)(sel->pending_first + sel->pending_steps - 1),
           sel->pending_dir ? "outward" : "inward",
           (unsigned long)sel->pending_steps, sel->pending_track);
    sel->pending_steps = 0;
}

/* Log a TRACK0 output change (the output itself was already set by the ISR). */
static void log_track0(selection_t *sel, int64_t time_us, const drive_status_t *st)
{
    if (st->track0 == sel->logged_track0) {
        return;
    }
    flush_pending_steps(sel);
    sel->logged_track0 = st->track0;
    print_timestamp(time_us);
    printf("TRACK0 -> %s\n", st->track0 ? "ACTIVE" : "INACTIVE");
}

static void selection_start(selection_t *sel, const monitor_event_t *ev, const char *suffix)
{
    bool logged_track0 = sel->logged_track0;

    *sel = (selection_t) {
        .selected = true,
        .start_us = ev->time_us,
        .logged_track0 = logged_track0,
        .logged_track = ev->drive.track,
    };
    for (int t = EV_MOTOR; t <= EV_WGATE; t++) {
        sel->last_level[t] = level_of(ev->inputs, signals[t].gpio);
    }
    if (sel->last_level[EV_WGATE] == 0) {
        sel->wgate_active = true;
        sel->wgate_periods = 1;
        sel->wgate_start_count = ev->wdata_count;
    }

    printf("\n");
    print_timestamp(ev->time_us);
    printf("DRIVE SELECTED (%s / GPIO%d)%s\n", EMU_SELECT_NAME, EMU_SELECT_LINE, suffix);
    print_snapshot(ev->time_us, ev->inputs);
    print_timestamp(ev->time_us);
    printf("Virtual track: %d\n", ev->drive.track);
    log_track0(sel, ev->time_us, &ev->drive);
}

static void selection_end(selection_t *sel, const monitor_event_t *ev, const char *suffix)
{
    flush_pending_steps(sel);

    bool wgate_still_active = sel->wgate_active;
    if (sel->wgate_active) {
        sel->wdata_pulses += ev->wdata_count - sel->wgate_start_count;
    }

    print_timestamp(ev->time_us);
    printf("DRIVE DESELECTED%s\n", suffix);
    if (sel->logged_track0 && !ev->drive.track0) {
        print_timestamp(ev->time_us);
        printf("TRACK0 -> RELEASED\n");
    }
    sel->logged_track0 = ev->drive.track0;
    printf("          Selected for: %lld ms\n", (ev->time_us - sel->start_us) / 1000);
    printf("          STEP pulses: %lu (inward: %lu, outward: %lu)\n",
           (unsigned long)sel->steps, (unsigned long)sel->steps_in,
           (unsigned long)sel->steps_out);
    printf("          Virtual track: %d\n", ev->drive.track);
    printf("          SIDE changes: %lu\n", (unsigned long)sel->side_changes);
    if (sel->wgate_periods == 0) {
        printf("          WGATE active: no\n");
    } else {
        printf("          WGATE active: yes (%lu times%s), WDATA pulses: %d\n",
               (unsigned long)sel->wgate_periods,
               wgate_still_active ? ", still active at deselect" : "",
               sel->wdata_pulses);
    }
    printf("\n");

    sel->selected = false;
}

static void handle_step(selection_t *sel, const monitor_event_t *ev)
{
    int dir = level_of(ev->inputs, PIN_FDD_DIR);
    int track = ev->drive.track;

    sel->steps++;
    if (dir) {
        sel->steps_out++;
    } else {
        sel->steps_in++;
    }

    if (sel->steps <= STEP_LINES_MAX) {
        print_timestamp(ev->time_us);
        printf("STEP #%lu %s -> virtual track %d%s\n", (unsigned long)sel->steps,
               dir ? "outward" : "inward", track,
               track == ev->track_before ? " (at limit, unchanged)" : "");
        if (sel->steps == STEP_LINES_MAX) {
            printf("          (further STEP pulses are summarised)\n");
        }
    } else {
        if (sel->pending_steps &&
            (sel->pending_dir != dir ||
             ev->time_us - sel->pending_time_us >= STEP_SUMMARY_MS * 1000LL)) {
            flush_pending_steps(sel);
        }
        if (sel->pending_steps == 0) {
            sel->pending_first = sel->steps;
            sel->pending_dir = dir;
            sel->pending_time_us = ev->time_us;
        }
        sel->pending_steps++;
        sel->pending_track = track;
    }

    sel->logged_track = track;
    log_track0(sel, ev->time_us, &ev->drive);
}

static void handle_level(selection_t *sel, const monitor_event_t *ev)
{
    const signal_t *s = &signals[ev->type];
    int level = level_of(ev->inputs, s->gpio);

    if (level == sel->last_level[ev->type]) {
        return;     /* e.g. both edges of a glitch queued */
    }
    sel->last_level[ev->type] = level;

    flush_pending_steps(sel);
    print_level(ev->time_us, s, level, "");

    if (ev->type == EV_SIDE) {
        sel->side_changes++;
    } else if (ev->type == EV_WGATE) {
        if (level == 0) {
            sel->wgate_active = true;
            sel->wgate_periods++;
            sel->wgate_start_count = ev->wdata_count;
        } else if (sel->wgate_active) {
            int pulses = ev->wdata_count - sel->wgate_start_count;
            sel->wgate_active = false;
            sel->wdata_pulses += pulses;
            printf("                 WDATA pulses received: %d\n", pulses);
        }
    }
}

static void handle_event(selection_t *sel, const monitor_event_t *ev)
{
    if (ev->type == EV_SELECT) {
        if (ev->drive.selected && !sel->selected) {
            selection_start(sel, ev, "");
        } else if (!ev->drive.selected && sel->selected) {
            selection_end(sel, ev, "");
        }
        return;
    }

    /* Queued by the ISR while selected, but the selection may have ended
     * in the meantime: shared signals never change our state then. */
    if (!sel->selected) {
        return;
    }

    if (ev->type == EV_STEP) {
        handle_step(sel, ev);
    } else {
        handle_level(sel, ev);
    }
}

/*
 * Idle check. Re-evaluates the drive outputs from the live levels (safety
 * net in case an edge interrupt was lost) and catches selection or level
 * changes whose event was lost.
 */
static void resync(selection_t *sel)
{
    if (uxQueueMessagesWaiting(event_queue) != 0) {
        return;
    }

    monitor_event_t ev = {
        .time_us = esp_timer_get_time(),
        .inputs = REG_READ(GPIO_IN_REG),
    };
    pcnt_unit_get_count(wdata_unit, &ev.wdata_count);
    drive_refresh(&ev.drive);

    if (ev.drive.selected != sel->selected) {
        if (ev.drive.selected) {
            selection_start(sel, &ev, "  [resync]");
        } else {
            selection_end(sel, &ev, "  [resync]");
        }
        return;
    }
    if (!sel->selected) {
        return;
    }

    log_track0(sel, ev.time_us, &ev.drive);
    for (int t = EV_MOTOR; t <= EV_WGATE; t++) {
        int level = level_of(ev.inputs, signals[t].gpio);
        if (level != sel->last_level[t]) {
            ev.type = t;
            handle_level(sel, &ev);
        }
    }
}

/* Report STEP pulses that did not move our head, once a burst has ended. */
static void report_ignored_steps(uint32_t *reported)
{
    uint32_t count = ignored_steps;

    if (count == *reported ||
        esp_timer_get_time() - ignored_step_time_us < IGNORED_REPORT_MS * 1000LL) {
        return;
    }

    drive_status_t st;
    drive_refresh(&st);
    print_timestamp(ignored_step_time_us);
    printf("%lu STEP pulses ignored (drive not %s); virtual track unchanged: %d\n",
           (unsigned long)(count - *reported), st.armed ? "selected" : "armed",
           st.track);
    *reported = count;
}

/*
 * Start-up guard: wait until the selected drive-select line has been HIGH
 * for ARM_HIGH_MS without interruption. Any LOW level or any edge seen by
 * the select interrupt restarts the time. Runs in task context only.
 */
static void wait_until_armed(void)
{
    uint32_t edges = select_edges;
    int64_t high_since = -1;

    printf("Waiting for %s to become inactive...\n", EMU_SELECT_NAME);

    while (true) {
        vTaskDelay(1);

        int64_t now = esp_timer_get_time();
        uint32_t e = select_edges;
        bool high = gpio_get_level(EMU_SELECT_LINE) == 1;

        if (!high || e != edges) {
            edges = e;
            high_since = high ? now : -1;
            continue;
        }
        if (high_since < 0) {
            high_since = now;
        }
        if (now - high_since >= ARM_HIGH_MS * 1000LL) {
            break;
        }
    }

    printf("%s HIGH for %d ms.\n", EMU_SELECT_NAME, ARM_HIGH_MS);
}

void input_monitor_run(void)
{
    selection_t sel = { 0 };
    uint32_t dropped_reported = 0;
    uint32_t ignored_reported = 0;

    event_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(monitor_event_t));
    assert(event_queue);

    inputs_init();
    wdata_counter_init();

    printf("\n========================================\n");
    printf(" ESP32-S3 Floppy Emulator\n");
    printf(" Atari Shugart Monitor + TRACK0 emulation\n");
    printf("========================================\n\n");
    verify_pins();
    printf("Active output: TRACK0 only (GPIO2). Max virtual track: %d.\n\n",
           DRIVE_MAX_TRACK);

    uint32_t inputs = REG_READ(GPIO_IN_REG);
    printf("Initial state:\n");
    print_snapshot(esp_timer_get_time(), inputs);
    printf("\n(Levels are raw; texts in brackets follow the Shugart active-low "
           "convention.)\n\n");

    /* Interrupts first, so the guard sees every select edge. The drive
     * is not armed yet, so TRACK0 stays released. */
    interrupts_init();
    wait_until_armed();

    drive_status_t st;
    drive_arm(&st);
    monitor_armed = true;
    printf("Emulator ARMED. Virtual track: %d (assumed at start-up)\n", st.track);
    printf("Shared signals are only logged while %s is LOW.\n\n", EMU_SELECT_NAME);
    ignored_reported = ignored_steps;   /* before arming: not interesting */

    while (true) {
        monitor_event_t ev;
        TickType_t wait = pdMS_TO_TICKS(sel.pending_steps ? STEP_SUMMARY_MS
                                                         : RESYNC_INTERVAL_MS);

        if (xQueueReceive(event_queue, &ev, wait)) {
            handle_event(&sel, &ev);
        } else {
            flush_pending_steps(&sel);
            resync(&sel);
            report_ignored_steps(&ignored_reported);
        }

        if (dropped_events != dropped_reported) {
            dropped_reported = dropped_events;
            printf("WARNING: event queue full, %lu events dropped in total\n",
                   (unsigned long)dropped_reported);
        }
    }
}
