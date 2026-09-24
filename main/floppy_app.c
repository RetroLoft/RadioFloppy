/*
 * RadioFloppy - read-only floppy emulation.
 *
 * Emulates drive B: (EMU_SELECT_LINE) with a .ST image from the external
 * SPI flash slot store (see disk_image.h):
 * MFM tracks pre-encoded in PSRAM, flux stream and INDEX from RMT, drive
 * logic in drive_emu.c (ISR context). This file only sets things up and
 * does the (compact) logging, which never touches the timing path.
 *
 * Writing is disabled: WPROT is asserted while selected, WGATE only
 * releases RDATA, WDATA is not even read.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"

#include "api.h"
#include "board_pins.h"
#include "buttons.h"
#include "disk_switch.h"
#include "disk_image.h"
#include "drive_config.h"
#include "drive_emu.h"
#include "flux_stream.h"
#include "status_led.h"
#include "step_sound.h"
#include "tests.h"
#include "wifi_net.h"

#define ARM_HIGH_MS         50      /* start-up guard, select line HIGH */
#define SHORT_SELECT_MS     20      /* shorter, without activity: a "poll" */
#define STEP_GROUP_MS       20      /* STEP pulses closer together: one line */
#define POLL_REPORT_MS      5000
#define SAFETY_REFRESH_MS   100

static void print_ts(int64_t t)
{
    printf("[%6lld.%03lld] ", t / 1000000, (t / 1000) % 1000);
}

/* Check in the registers that the outputs are released and inputs are inputs. */
static bool verify_outputs_released(void)
{
    static const gpio_num_t plain[] = {
        PIN_FDD_TRK0, PIN_FDD_WPROT, PIN_FDD_READY, PIN_FDD_DSKCHG,
    };
    static const gpio_num_t rmt[] = { PIN_FDD_INDEX, PIN_FDD_RDATA };
    uint64_t level = REG_READ(GPIO_OUT_REG) | ((uint64_t)REG_READ(GPIO_OUT1_REG) << 32);
    uint64_t enable = REG_READ(GPIO_ENABLE_REG) | ((uint64_t)REG_READ(GPIO_ENABLE1_REG) << 32);
    bool ok = true;

    for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
        gpio_num_t p = plain[i];
        if (!(enable & (1ULL << p)) || (level & (1ULL << p))) {
            printf("ERROR: GPIO%d is not an output driving LOW\n", p);
            ok = false;
        }
    }
    for (size_t i = 0; i < sizeof(rmt) / sizeof(rmt[0]); i++) {
        gpio_num_t p = rmt[i];
        uint32_t sel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * p) & GPIO_FUNC0_OUT_SEL_M;
        if (!(enable & (1ULL << p)) || (level & (1ULL << p)) || sel != SIG_GPIO_OUT_IDX) {
            printf("ERROR: GPIO%d is not parked LOW (out_sel %lu)\n", p, (unsigned long)sel);
            ok = false;
        }
    }
    bool in_ok = REG_GET_BIT(GPIO_PIN_MUX_REG[EMU_SELECT_LINE], FUN_IE) &&
                 !(enable & (1ULL << EMU_SELECT_LINE));
    printf("Outputs released: %s (INDEX/RDATA parked, TRACK0/WPROT/READY/DSKCHG LOW)\n",
           ok ? "YES, verified" : "NO");
    printf("Select line: %s / GPIO%d - %s\n", EMU_SELECT_NAME, EMU_SELECT_LINE,
           in_ok ? "input OK" : "ERROR: not an input");
    return ok && in_ok;
}

static void wait_until_armed(void)
{
    uint32_t edges = drive_select_edges();
    int64_t high_since = -1;

    printf("Waiting for %s to become inactive...\n", EMU_SELECT_NAME);
    while (true) {
        vTaskDelay(1);
        int64_t now = esp_timer_get_time();
        uint32_t e = drive_select_edges();
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

/* ---- Logging state ---------------------------------------------------- */

typedef struct {
    bool selected;
    bool header_printed;    /* long enough / active enough to be shown */
    int64_t start_us;
    uint32_t start_revs;
    drive_status_t start_st;
    drive_status_t last_st; /* last logged output state */
    uint32_t steps;
    uint32_t side_changes;
    bool wgate_seen;
    /* STEP pulses grouped into one line */
    uint32_t pending_steps;
    int64_t pending_time_us;
    int pending_cyl;
    /* short selections without activity */
    uint32_t polls;
    int64_t polls_since_us;
} log_state_t;

static void flush_steps(log_state_t *ls)
{
    if (ls->pending_steps == 0) {
        return;
    }
    print_ts(ls->pending_time_us);
    if (ls->pending_steps == 1) {
        printf("track -> %d\n", ls->pending_cyl);
    } else {
        printf("track -> %d (%lu steps)\n", ls->pending_cyl, (unsigned long)ls->pending_steps);
    }
    ls->pending_steps = 0;
}

static void log_outputs(log_state_t *ls, int64_t t, const drive_status_t *st)
{
    if (st->track0 != ls->last_st.track0) {
        print_ts(t);
        printf("TRACK0 %s\n", st->track0 ? "active" : "inactive");
    }
    if (st->motor != ls->last_st.motor) {
        print_ts(t);
        printf("MOTOR %s\n", st->motor ? "on" : "off");
    }
    if (st->rdata != ls->last_st.rdata) {
        print_ts(t);
        printf("RDATA %s\n", st->rdata ? "started" : "stopped");
    }
    ls->last_st = *st;
}

static void print_header(log_state_t *ls)
{
    const drive_status_t *st = &ls->start_st;

    ls->header_printed = true;
    printf("\n");
    print_ts(ls->start_us);
    printf("B: selected (%s)\n", EMU_SELECT_NAME);
    print_ts(ls->start_us);
    printf("track=%d side=%d\n", st->cyl, st->side);
    print_ts(ls->start_us);
    printf("TRACK0 %s, WPROT %s, MOTOR %s\n", st->track0 ? "active" : "inactive",
           st->wprot ? "active" : "inactive", st->motor ? "on" : "off");
    if (st->rdata) {
        print_ts(ls->start_us);
        printf("RDATA started\n");
    }
    ls->last_st = *st;
}

static void selection_start(log_state_t *ls, const drive_event_t *ev)
{
    ls->selected = true;
    ls->header_printed = false;
    ls->start_us = ev->time_us;
    ls->start_revs = flux_revolutions();
    ls->start_st = ev->st;
    ls->steps = 0;
    ls->side_changes = 0;
    ls->wgate_seen = false;
    ls->pending_steps = 0;
}

static void selection_end(log_state_t *ls, const drive_event_t *ev)
{
    int64_t dur_us = ev->time_us - ls->start_us;

    ls->selected = false;
    if (!ls->header_printed) {
        if (ls->polls++ == 0) {
            ls->polls_since_us = ev->time_us;
        }
        return;
    }
    flush_steps(ls);
    print_ts(ev->time_us);
    printf("B: deselected%s\n", ls->last_st.rdata ? ", RDATA stopped" : "");
    printf("          %lld ms, %lu steps, %lu side changes, %lu revolutions, "
           "track=%d side=%d, WGATE %s\n",
           dur_us / 1000, (unsigned long)ls->steps, (unsigned long)ls->side_changes,
           (unsigned long)(flux_revolutions() - ls->start_revs),
           ev->st.cyl, ev->st.side, ls->wgate_seen ? "seen (ignored)" : "no");
    ls->last_st = ev->st;
}

static void handle_event(log_state_t *ls, const drive_event_t *ev)
{
    if (ev->type == DRV_EV_SELECT) {
        if (ev->st.selected && !ls->selected) {
            selection_start(ls, ev);
        } else if (!ev->st.selected && ls->selected) {
            selection_end(ls, ev);
        }
        return;
    }
    if (!ls->selected) {
        return;     /* queued while selected, handled after deselect */
    }
    if (!ls->header_printed) {
        print_header(ls);
    }
    if (ev->type != DRV_EV_STEP) {
        flush_steps(ls);    /* keep the log in time order */
    }

    switch (ev->type) {
    case DRV_EV_STEP:
        ls->steps++;
        if (ls->pending_steps &&
            ev->time_us - ls->pending_time_us > STEP_GROUP_MS * 1000LL) {
            flush_steps(ls);
        }
        if (ls->pending_steps++ == 0) {
            ls->pending_time_us = ev->time_us;
        }
        ls->pending_cyl = ev->st.cyl;
        break;
    case DRV_EV_SIDE:
        if (ev->st.side != ls->last_st.side) {
            flush_steps(ls);
            ls->side_changes++;
            print_ts(ev->time_us);
            printf("side -> %d\n", ev->st.side);
        }
        break;
    case DRV_EV_WGATE:
        if (ev->st.wgate && !ls->wgate_seen) {
            flush_steps(ls);
            print_ts(ev->time_us);
            printf("WGATE active - ignored (read-only)\n");
        }
        ls->wgate_seen |= ev->st.wgate;
        break;
    default:
        break;
    }
    if (ev->type != DRV_EV_STEP) {
        log_outputs(ls, ev->time_us, &ev->st);
    } else {
        /* Only report output changes, the track itself is in the STEP line. */
        drive_status_t st = ev->st;
        if (st.track0 != ls->last_st.track0 || st.rdata != ls->last_st.rdata) {
            flush_steps(ls);
            log_outputs(ls, ev->time_us, &st);
        }
        ls->last_st = st;
    }
}

void floppy_emu_run(void)
{
    printf("\n========================================\n");
    printf(" RadioFloppy\n");
    printf(" Read-only drive %s\n", EMU_SELECT_LINE == EMU_DS1 ? "B: (DS1)" : "(DS0)");
    printf("========================================\n\n");

    if (disk_image_init() != ESP_OK) {
        printf("Disk buffers not available - emulator stays disabled.\n");
        return;
    }
    disk_info_t disk;
    disk_get_current(&disk);
    printf("Floppy image: %s (%s)\n", disk.name,
           disk.source == DISK_SRC_FLASH ? "EXTERNAL SPI FLASH" : "none");
    if (disk.heads) {
        mfm_layout_t l = mfm_layout(disk.sectors);
        printf("Geometry: %d/%d/%d/%d%s\n", disk.cylinders, disk.heads, disk.sectors,
               MFM_SECTOR_SIZE, disk.heads == 1 ? " (side 1 unformatted)" : "");
        printf("MFM: 250 kbit/s, %lu bitcells/track, GAP3 %d, interleave %d, skew %s\n",
               (unsigned long)l.cells, l.gap3, l.interleave, MFM_USE_TOS_SKEW ? "TOS" : "none");
    }
    printf("RPM: 300 (200 ms, INDEX %d ms)\n", INDEX_PULSE_MS);
    printf("Read-only: YES\n");

    /* Before the flux stream: the one-time PHY calibration write to the
     * internal flash must not happen while RMT is streaming. */
    bool wifi = wifi_net_start() == ESP_OK;

    step_sound_init();              /* buzzer off before any interrupt */
    drive_init();                   /* inputs + interrupts, not armed */
    if (flux_stream_init() != ESP_OK) {
        printf("RMT failed - emulator stays disabled.\n");
        return;
    }
    printf("RMT ready (%d MHz, RDATA pulse %d.%d us, stream running, outputs gated).\n",
           FLUX_RESOLUTION_HZ / 1000000, FLUX_PULSE_TICKS / 10, FLUX_PULSE_TICKS % 10);
    status_led_init();              /* indication only: failure is not fatal */

    if (!verify_outputs_released()) {
        printf("Output check failed - emulator stays disabled.\n");
        return;
    }
    disk_switch_init();
    if (wifi) {
        api_start();
    }
    buttons_start();
    printf("Ready.\n\n");

    wait_until_armed();
    drive_status_t st;
    drive_arm(&st);
    printf("Emulator ARMED. Virtual track: %d\n\n", st.cyl);

    log_state_t ls = { .last_st = st };
    uint32_t ignored_reported = drive_ignored_steps();
    int64_t last_refresh = esp_timer_get_time();

    while (true) {
        drive_event_t ev;
        bool waiting = (ls.selected && !ls.header_printed) || ls.pending_steps;
        TickType_t wait = pdMS_TO_TICKS(waiting ? 5 : SAFETY_REFRESH_MS);

        if (xQueueReceive(drive_events(), &ev, wait)) {
            handle_event(&ls, &ev);
        }

        int64_t now = esp_timer_get_time();

        /* A selection that lasts long enough is shown even without events. */
        if (ls.selected && !ls.header_printed &&
            now - ls.start_us >= SHORT_SELECT_MS * 1000LL) {
            print_header(&ls);
        }
        if (ls.pending_steps && now - ls.pending_time_us > STEP_GROUP_MS * 1000LL) {
            flush_steps(&ls);
        }
        if (ls.polls && now - ls.polls_since_us >= POLL_REPORT_MS * 1000LL) {
            print_ts(now);
            printf("B: %lu short selections without activity (polls)\n",
                   (unsigned long)ls.polls);
            ls.polls = 0;
        }

        /* Safety net: re-evaluate the outputs from the live levels. */
        if (now - last_refresh >= SAFETY_REFRESH_MS * 1000LL) {
            last_refresh = now;
            drive_refresh(&st);
            uint32_t ignored = drive_ignored_steps();
            if (ignored != ignored_reported && !ls.selected) {
                print_ts(now);
                printf("%lu STEP pulses for another drive ignored (track stays %d)\n",
                       (unsigned long)(ignored - ignored_reported), st.cyl);
                ignored_reported = ignored;
            }
        }
    }
}
