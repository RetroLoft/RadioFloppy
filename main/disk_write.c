/*
 * Writing to the emulated floppy. See disk_write.h.
 *
 * Two tasks on core 1 (away from the floppy ISRs and the flux stream on
 * core 0): the receive task (high priority) re-arms the RMT receiver,
 * decodes and applies sectors within a few milliseconds; the save task
 * (low priority) does the slow flash work and decides write protection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/rmt_rx.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "disk_image.h"
#include "disk_write.h"
#include "drive_emu.h"
#include "flux_stream.h"
#include "image_store.h"
#include "machine.h"
#include "mfm_track.h"

#define RX_RESOLUTION_HZ    10000000    /* 0.1 us per tick */
#define RX_TICK_NS          100
#define RX_SYMBOLS          8192        /* one sector write is about 4500 transitions */
#define RX_MIN_NS           100         /* shorter pulses are glitches */
#define RX_IDLE_NS          16000       /* WDATA quiet this long: the write ended */
#define REV_US              (FLUX_REV_TICKS / (RX_RESOLUTION_HZ / 1000000))    /* 200000 */
/* WD1772 Write Sector: WGATE on 22 bytes (352 cells) after the ID field. */
#define WRITE_OFFSET_MIN    100
#define WRITE_OFFSET_MAX    1200
#define RETRY_MS            30000       /* retry a failed save */

static rmt_channel_handle_t rx_chan;
static rmt_symbol_word_t *rx_buf;       /* DMA target, internal RAM */
static rmt_symbol_word_t *rx_copy;      /* decoded from here (PSRAM) */
static uint16_t *intervals;
static QueueHandle_t rx_queue;
static bool rx_ready;

static TaskHandle_t save_task;
static SemaphoreHandle_t save_lock;
static portMUX_TYPE st_lock = portMUX_INITIALIZER_UNLOCKED;
static write_status_t status = { .state = WRITE_READ_ONLY, .reason = "starting" };
static volatile int64_t last_write_us;
static volatile bool save_error;
static volatile bool saving;
static int64_t last_try_us;

static const rmt_receive_config_t rx_cfg = {
    .signal_range_min_ns = RX_MIN_NS,
    .signal_range_max_ns = RX_IDLE_NS,
};

const char *disk_write_state_name(write_state_t s)
{
    switch (s) {
    case WRITE_WRITABLE: return "writable";
    case WRITE_PENDING:  return "pending";
    case WRITE_SAVING:   return "saving";
    case WRITE_ERROR:    return "error";
    default:             return "read_only";
    }
}

/* ---- Write protection --------------------------------------------------------- */

/* NULL: the inserted disk may be written; else why not. */
static const char *protect_reason(void)
{
    disk_info_t d;
    image_record_t r;
    store_usage_t u;

    if (!rx_ready) {
        return "write receiver not available";
    }
    if (machine_active() != MACHINE_ATARI) {
        return "computer not supported";
    }
    disk_get_current(&d);
    if (d.source == DISK_SRC_NONE) {
        return "no disk";
    }
    if (d.source != DISK_SRC_FLASH) {
        return "temporary disk (PSRAM)";
    }
    if (d.image_changed) {
        return "image replaced or deleted since it was loaded";
    }
    if (!image_store_get(d.image_id, &r) || !image_store_is_valid(&r)) {
        return "image not found";
    }
    if (!image_read_write(&r)) {
        return "image is read-only";
    }
    if (image_format(&r) != IMG_FMT_ST) {
        return "format cannot be written";
    }
    if (!disk_writable_data()) {
        return "not enough memory to keep the sectors";
    }
    if (image_store_state() != STORE_VALID) {
        return "image storage not usable";
    }
    image_store_usage(&u);
    if (u.blocks_free < r.block_count) {
        return "not enough free storage to save changes";
    }
    if (save_error) {
        return "saving failed";
    }
    return NULL;
}

static void update_protection(void)
{
    const char *why = protect_reason();
    bool on = why == NULL;

    if (on != drive_writable()) {
        drive_set_writable(on);
        printf("Write: disk %s%s%s\n", on ? "WRITABLE" : "write-protected",
               on ? "" : " - ", on ? "" : why);
    }
    portENTER_CRITICAL(&st_lock);
    status.writable = on;
    status.reason = on ? "" : why;
    portEXIT_CRITICAL(&st_lock);
}

void disk_write_refresh(void)
{
    if (save_task) {
        xTaskNotify(save_task, 1, eSetBits);
    } else {
        update_protection();
    }
}

/* ---- Receiving and decoding ----------------------------------------------------- */

static bool IRAM_ATTR rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ev,
                              void *arg)
{
    BaseType_t woken = pdFALSE;
    size_t n = ev->num_symbols;
    xQueueSendFromISR(rx_queue, &n, &woken);
    return woken == pdTRUE;
}

/* RMT symbols -> times between successive falling edges (write pulses). */
static int to_intervals(const rmt_symbol_word_t *sym, size_t n)
{
    uint32_t t = 0;
    int64_t last = -1;
    int prev = 1;                       /* WDATA idles HIGH */
    int ni = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned level[2] = { sym[i].level0, sym[i].level1 };
        unsigned dur[2] = { sym[i].duration0, sym[i].duration1 };
        for (int k = 0; k < 2; k++) {
            if (level[k] == 0 && prev == 1) {
                if (last >= 0 && ni < RX_SYMBOLS) {
                    uint32_t d = t - (uint32_t)last;
                    intervals[ni++] = d > 0xffff ? 0xffff : d;
                }
                last = t;
            }
            prev = level[k];
            if (dur[k] == 0) {
                return ni;              /* end marker */
            }
            t += dur[k];
        }
    }
    return ni;
}

static void reject(const char *why, const drive_write_start_t *ws)
{
    portENTER_CRITICAL(&st_lock);
    status.writes_rejected++;
    portEXIT_CRITICAL(&st_lock);
    printf("Write: rejected on C%d H%d - %s\n", ws->cyl, ws->side, why);
}

static void handle_write(size_t nsym)
{
    static uint32_t last_seq;
    static uint8_t data[MFM_SECTOR_SIZE];
    drive_write_start_t ws;
    disk_info_t d;
    uint8_t mark;

    drive_get_write_start(&ws);
    if (ws.seq == last_seq) {
        return;                         /* no WGATE: noise on WDATA */
    }
    last_seq = ws.seq;
    if (!ws.writable) {
        reject("disk was write-protected", &ws);
        return;
    }
    if (esp_timer_get_time() - ws.time_us > 500000) {
        reject("stale", &ws);
        return;
    }
    int n = to_intervals(rx_copy, nsym);
    mfm_wr_result_t r = mfm_decode_write(intervals, n, RX_TICK_NS, &mark, data);
    if (r != MFM_WR_OK) {
        reject(r == MFM_WR_BAD_CRC ? "CRC error" : r == MFM_WR_SHORT ? "incomplete data"
               : r == MFM_WR_ID_FIELD ? "track format (not supported)" : "no data mark", &ws);
        return;
    }

    /* Which sector: the ID field that passed just before WGATE. */
    int64_t phase = ws.time_us - ws.index_us;
    if (ws.index_us == 0 || phase < 0 || phase > REV_US + 5000) {
        reject("no INDEX reference", &ws);
        return;
    }
    disk_get_current(&d);
    if (d.source != DISK_SRC_FLASH || !d.sectors) {
        reject("no writable disk", &ws);
        return;
    }
    mfm_layout_t layout = mfm_layout(d.sectors);
    uint32_t pos = (uint32_t)((phase % REV_US) * layout.cells / REV_US);
    int sector = 0;
    int off = mfm_sector_at(&layout, ws.cyl, ws.side, pos, &sector);
    if (off < WRITE_OFFSET_MIN || off > WRITE_OFFSET_MAX) {
        char why[48];
        snprintf(why, sizeof(why), "position (%d cells after an ID)", off);
        reject(why, &ws);
        return;
    }
    esp_err_t err = disk_write_sector(ws.gen, ws.cyl, ws.side, sector, data);
    if (err != ESP_OK) {
        reject(err == ESP_ERR_INVALID_STATE ? "disk changed" : "outside the image", &ws);
        return;
    }
    last_write_us = esp_timer_get_time();
    portENTER_CRITICAL(&st_lock);
    status.sectors_written++;
    portEXIT_CRITICAL(&st_lock);
    printf("Write: C%d H%d S%d%s (offset %d)\n", ws.cyl, ws.side, sector,
           mark == 0xf8 ? " deleted-data mark" : "", off);
    xTaskNotify(save_task, 1, eSetBits);
}

static bool arm(void)
{
    if (rmt_receive(rx_chan, rx_buf, RX_SYMBOLS * sizeof(rmt_symbol_word_t), &rx_cfg) == ESP_OK) {
        return true;
    }
    printf("Write: receiver failed - disks stay write-protected\n");
    rx_ready = false;
    disk_write_refresh();
    return false;
}

static void rx_task(void *arg)
{
    size_t n;

    if (!arm()) {
        vTaskDelete(NULL);
    }
    while (true) {
        xQueueReceive(rx_queue, &n, portMAX_DELAY);
        if (n > RX_SYMBOLS) {
            n = RX_SYMBOLS;
        }
        memcpy(rx_copy, rx_buf, n * sizeof(rmt_symbol_word_t));
        /* Re-arm before decoding: the next write can follow soon. */
        if (!arm()) {
            vTaskDelete(NULL);
        }
        if (n >= 64) {                  /* a sector write has thousands of pulses */
            handle_write(n);
        }
    }
}

/* ---- Saving ------------------------------------------------------------------------ */

static esp_err_t save_now(void)
{
    disk_snapshot_t snap;
    esp_err_t err;

    xSemaphoreTake(save_lock, portMAX_DELAY);
    err = disk_snapshot(&snap);
    if (err == ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(save_lock);
        return ESP_OK;                  /* nothing to save */
    }
    saving = true;
    last_try_us = esp_timer_get_time();
    if (err == ESP_OK) {
        int64_t t0 = esp_timer_get_time();
        uint32_t crc = esp_rom_crc32_le(0, snap.data, snap.size);
        err = image_store_commit_blocks(snap.image_id, snap.mask, snap.data, snap.size, crc);
        free(snap.data);
        if (err == ESP_OK) {
            printf("Write: saved image %u (%d block(s), %lld ms)\n", snap.image_id,
                   __builtin_popcount(snap.mask), (esp_timer_get_time() - t0) / 1000);
        } else {
            disk_snapshot_failed(&snap);
        }
    }
    saving = false;
    portENTER_CRITICAL(&st_lock);
    if (err == ESP_OK) {
        status.saves++;
        status.last_save_us = esp_timer_get_time();
        status.error[0] = 0;
    }
    portEXIT_CRITICAL(&st_lock);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s", err == ESP_ERR_NO_MEM ? "not enough free storage"
                 : err == ESP_ERR_NOT_FOUND ? "image not found" : esp_err_to_name(err));
        portENTER_CRITICAL(&st_lock);
        memcpy(status.error, msg, sizeof(status.error));
        portEXIT_CRITICAL(&st_lock);
        printf("Write: SAVING FAILED (%s) - disk write-protected, changes kept in memory\n", msg);
    }
    save_error = err != ESP_OK;
    xSemaphoreGive(save_lock);
    update_protection();
    return err;
}

static void save_task_fn(void *arg)
{
    while (true) {
        xTaskNotifyWait(0, UINT32_MAX, NULL, pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();
        drive_status_t ds;
        drive_peek(&ds);
        if (disk_dirty() && !save_error && !ds.wgate &&
            now - last_write_us >= SAVE_DELAY_MS * 1000LL) {
            save_now();
        } else if (disk_dirty() && save_error && now - last_try_us >= RETRY_MS * 1000LL) {
            save_now();                 /* e.g. storage was freed meanwhile */
        }
        if (!disk_dirty() && save_error) {
            save_error = false;         /* nothing left to save */
        }
        update_protection();
    }
}

esp_err_t disk_write_flush(void)
{
    if (!save_lock) {
        return ESP_OK;                  /* writing not initialised: never dirty */
    }
    /* Protect first, then let a write that is being received finish. */
    drive_set_writable(false);
    vTaskDelay(pdMS_TO_TICKS(60));
    esp_err_t err = disk_dirty() ? save_now() : ESP_OK;
    if (err == ESP_OK && disk_dirty()) {
        err = save_now();
    }
    update_protection();
    return err;
}

bool disk_write_unsaved(void)
{
    return disk_dirty();
}

void disk_write_status(write_status_t *st)
{
    portENTER_CRITICAL(&st_lock);
    *st = status;
    portEXIT_CRITICAL(&st_lock);
    bool dirty = disk_dirty();
    st->state = saving ? WRITE_SAVING
                : save_error && dirty ? WRITE_ERROR
                : dirty ? WRITE_PENDING
                : st->writable ? WRITE_WRITABLE : WRITE_READ_ONLY;
}

/* ---- Start-up ------------------------------------------------------------------------ */

esp_err_t disk_write_init(void)
{
    save_lock = xSemaphoreCreateMutex();
    rx_queue = xQueueCreate(4, sizeof(size_t));
    rx_buf = heap_caps_aligned_alloc(64, RX_SYMBOLS * sizeof(rmt_symbol_word_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    rx_copy = heap_caps_malloc(RX_SYMBOLS * sizeof(rmt_symbol_word_t), MALLOC_CAP_SPIRAM);
    intervals = heap_caps_malloc(RX_SYMBOLS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);

    esp_err_t err = ESP_ERR_NO_MEM;
    if (save_lock && rx_queue && rx_buf && rx_copy && intervals) {
        const rmt_rx_channel_config_t cfg = {
            .gpio_num = PIN_FDD_WDATA,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = RX_RESOLUTION_HZ,
            .mem_block_symbols = RX_SYMBOLS,   /* with DMA: sets the descriptor count */
            .flags.with_dma = 1,
        };
        err = rmt_new_rx_channel(&cfg, &rx_chan);
        if (err == ESP_OK) {
            const rmt_rx_event_callbacks_t cbs = { .on_recv_done = rx_done };
            err = rmt_rx_register_event_callbacks(rx_chan, &cbs, NULL);
        }
        if (err == ESP_OK) {
            err = rmt_enable(rx_chan);
        }
    }
    rx_ready = err == ESP_OK;
    printf("Write: receiver %s (WDATA GPIO%d, RMT RX + DMA, %u symbols)\n",
           rx_ready ? "ready" : esp_err_to_name(err), PIN_FDD_WDATA, RX_SYMBOLS);
    if (rx_ready) {
        xTaskCreatePinnedToCore(rx_task, "fdd_write", 4096, NULL, 12, NULL, 1);
    }
    xTaskCreatePinnedToCore(save_task_fn, "fdd_save", 4096, NULL, 3, &save_task, 1);
    return err;
}
