/*
 * The active floppy. See disk_image.h.
 *
 * Images are raw .ST sector dumps (79-84 cylinders, 9-11 x 512 bytes, 1 or
 * 2 sides) or HFE files, from the SD card (or the copy of the last one in
 * the flash, disk_copy.h) or a temporary upload. Every
 * track is encoded once into a PSRAM track buffer (80 x 2 x 12500 bytes),
 * so a STEP or SIDE change never waits for encoding; side 1 of a
 * single-sided image is an unformatted track. New images go into the
 * inactive buffer and are verified before the buffers are swapped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "disk_image.h"
#include "drive_emu.h"
#include "flux_stream.h"
#include "hfe.h"
#include "st_image.h"
#include "ext_flash.h"
#include "disk_title.h"
#include "disk_copy.h"
#include "esp_rom_crc.h"
#include "flash_layout.h"
#include "sd_card.h"
#include "settings.h"

uint8_t *volatile disk_tracks;
volatile uint32_t disk_track_cells = MFM_TRACK_CELLS;
volatile uint8_t disk_tracks_kind = DISK_TRACKS_MFM;

static uint8_t *track_buf[2];       /* A/B track buffers in PSRAM */
static int active_buf;              /* index of the buffer in use */
static disk_info_t current = { .source = DISK_SRC_NONE };
static portMUX_TYPE info_lock = portMUX_INITIALIZER_UNLOCKED;

/* .ST order: cylinder, then head, then sectors 1..9. */
static const uint8_t *sector_data(const uint8_t *raw, const disk_info_t *g, int cyl, int head)
{
    return raw + (size_t)(cyl * g->heads + head) * g->sectors * MFM_SECTOR_SIZE;
}

static uint8_t *track_ptr(uint8_t *buf, int cyl, int head)
{
    return buf + (size_t)(cyl * DISK_MAX_HEADS + head) * MFM_MAX_BYTES;
}

static uint32_t prepared_cells;     /* track length in the inactive buffer */
static uint8_t prepared_kind;       /* DISK_TRACKS_* in the inactive buffer */

/*
 * Sector data of the active disk, kept for writing (writable disks only;
 * none for now): written sectors are patched here, their track is encoded
 * again, and the changed 64 KiB regions are marked in dirty_mask until
 * they are saved.
 */
static SemaphoreHandle_t raw_lock;
static uint8_t *active_raw;         /* NULL: disk not writable */
static uint8_t *prepared_raw;       /* copy taken by disk_prepare() */
static disk_info_t active_geo;      /* geometry and id belonging to active_raw */
static uint32_t dirty_mask;         /* bit i: 64 KiB region i changed */
#define DIRTY_REGION    (64u * 1024u)
volatile uint32_t disk_media_gen;   /* +1 per disk change */

/*
 * Encode all tracks of raw into buf. Side 1 of a single-sided image and the
 * cylinders beyond the image up to DISK_MAX_CYLS are unformatted tracks
 * of the same length. raw NULL: no disk, all tracks unformatted.
 */
static esp_err_t build_tracks(uint8_t *buf, const uint8_t *raw, const disk_info_t *g,
                              uint32_t *cells_out)
{
    static uint8_t *blank;          /* unformatted track, rebuilt per length */
    static uint32_t blank_cells;
    mfm_layout_t layout = mfm_layout(raw ? g->sectors : MFM_MIN_SECTORS);
    uint8_t *work = heap_caps_malloc(MFM_MAX_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!blank) {
        blank = heap_caps_malloc(MFM_MAX_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!work || !blank) {
        free(work);
        return ESP_ERR_NO_MEM;
    }
    if (blank_cells != layout.cells) {
        mfm_build_blank_track(blank, layout.cells);
        blank_cells = layout.cells;
    }
    for (int cyl = 0; cyl < DISK_MAX_CYLS; cyl++) {
        for (int head = 0; head < DISK_MAX_HEADS; head++) {
            const uint8_t *src = blank;
            if (raw && cyl < g->cylinders && head < g->heads) {
                mfm_build_track(work, &layout, sector_data(raw, g, cyl, head), cyl, head);
                src = work;
            }
            memcpy(track_ptr(buf, cyl, head), src, layout.cells / 8);
        }
    }
    free(work);
    *cells_out = layout.cells;
    return ESP_OK;
}

/*
 * Background check of the active disk: decode every track again and
 * compare with the image. Runs after a switch on its own copy of the
 * image; a new switch aborts it instead of waiting for it.
 */
static struct {
    uint8_t *raw;               /* own copy, freed by the task */
    disk_info_t geo;
    const uint8_t *tracks;
} verify_job;
static volatile bool verify_running;
static volatile bool verify_abort;

static void verify_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    int bad = 0, done = 0;

    const disk_info_t *g = &verify_job.geo;
    mfm_layout_t layout = mfm_layout(g->sectors);

    for (int cyl = 0; cyl < g->cylinders && !verify_abort; cyl++) {
        for (int head = 0; head < g->heads; head++) {
            const uint8_t *t = track_ptr((uint8_t *)verify_job.tracks, cyl, head);
            if (mfm_verify_track(t, &layout, sector_data(verify_job.raw, g, cyl, head),
                                 cyl, head) != g->sectors) {
                bad++;
            }
            done++;
        }
        vTaskDelay(1);
    }
    if (verify_abort) {
        printf("MFM verify: aborted (disk change or store)\n");
    } else {
        printf("MFM verify: %d/%d tracks OK (%lld ms, background)%s\n", done - bad, done,
               (esp_timer_get_time() - t0) / 1000, bad ? " - ERRORS" : "");
    }
    free(verify_job.raw);
    verify_running = false;
    vTaskDelete(NULL);
}

static void verify_stop(void)
{
    if (verify_running) {
        verify_abort = true;
        while (verify_running) {
            vTaskDelay(1);      /* at most one track (~12 ms) */
        }
        verify_abort = false;
    }
}

void disk_verify_cancel(void)
{
    verify_stop();
}

/* Check the active tracks against raw in the background (raw is copied). */
static void verify_start(const uint8_t *raw, const disk_info_t *g)
{
    uint32_t size = g->size;
    verify_stop();
    if (!DISK_BACKGROUND_VERIFY) {
        return;
    }
    uint8_t *copy = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        printf("MFM verify: skipped (no memory)\n");
        return;
    }
    memcpy(copy, raw, size);
    verify_job.raw = copy;
    verify_job.geo = *g;
    verify_job.tracks = track_buf[active_buf];
    verify_running = true;
    /* Lowest priority, on the core that does not run the GPIO/RMT ISRs. */
    xTaskCreatePinnedToCore(verify_task, "mfm_verify", 4096, NULL, 1, NULL, 1);
}

static void (*change_cb)(void);

void disk_set_change_callback(void (*cb)(void))
{
    change_cb = cb;
}

static void set_current(const disk_info_t *info)
{
    portENTER_CRITICAL(&info_lock);
    current = *info;
    portEXIT_CRITICAL(&info_lock);
    if (change_cb) {
        change_cb();
    }
}

void disk_get_current(disk_info_t *info)
{
    portENTER_CRITICAL(&info_lock);
    *info = current;
    portEXIT_CRITICAL(&info_lock);
}

void disk_note_sd_changed(const char *path, bool folder)
{
    size_t n = strlen(path);

    portENTER_CRITICAL(&info_lock);
    if (current.source == DISK_SRC_SD &&
        (folder ? strncmp(current.sd_path, path, n) == 0 && current.sd_path[n] == '/'
                : strcmp(current.sd_path, path) == 0)) {
        current.image_changed = true;
    }
    portEXIT_CRITICAL(&info_lock);
}

/* The old image library's catalogs (before the SD card): erased once. */
static void erase_old_library(void)
{
    static const uint32_t magics[] = { 0x4c494652 /* "RFIL" */, 0x4c534652 /* "RFSL" */,
                                       0x54434652 /* "RFCT" */ };
    static const uint32_t addrs[] = { 0x000000, 0x006000, 0x001000, 0x002000, 0x003000 };
    bool found = false;

    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]) && !found; i++) {
        uint32_t m = 0;
        ext_flash_read(addrs[i], &m, sizeof(m));
        for (size_t k = 0; k < sizeof(magics) / sizeof(magics[0]); k++) {
            found |= m == magics[k];
        }
    }
    if (found) {
        esp_err_t err = ext_flash_erase(RF_OLD_CATALOGS, RF_OLD_CATALOGS_SIZE);
        printf("Flash: old image library erased%s (images live on the SD card now)\n",
               err == ESP_OK ? "" : " - FAILED");
    }
}

static bool is_hfe_name(const char *path)
{
    size_t n = strlen(path);
    return n >= 4 && strcasecmp(path + n - 4, ".hfe") == 0;
}

/* ---- HFE: packed tracks, read as a stream (SD card or the flash copy) -------- */

typedef struct {
    hfe_stream_t stream;
    uint32_t crc;
} hfe_load_t;

static bool hfe_feed(void *ctx, const uint8_t *data, uint32_t len)
{
    hfe_load_t *l = ctx;
    l->crc = esp_rom_crc32_le(l->crc, data, len);
    return hfe_stream_feed(&l->stream, data, len) == HFE_OK;
}

/*
 * The HFE file info->sd_path (from the card, or from the flash copy when
 * from_copy) -> packed tracks in buf. Fills in size, CRC and geometry.
 * ESP_ERR_NOT_FOUND: no card or the card cannot be read (the copy may help).
 */
static esp_err_t build_hfe_from(uint8_t *buf, disk_info_t *info, bool from_copy)
{
    static uint8_t chunk[8192];
    disk_copy_info_t ci;
    uint32_t size = 0;
    bool dir = false;

    if (from_copy) {
        if (!disk_copy_get(&ci)) {
            return ESP_FAIL;
        }
        size = ci.size;
    } else {
        sd_result_t r = sd_card_stat(info->sd_path, &dir, &size);
        if (r != SD_OK || dir) {
            return r == SD_NO_CARD || r == SD_IO ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
    }
    hfe_load_t *l = heap_caps_malloc(sizeof(*l), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *region = heap_caps_malloc(HFE_REGION_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!l || !region) {
        free(l);
        free(region);
        return ESP_ERR_NO_MEM;
    }
    int64_t t0 = esp_timer_get_time();
    hfe_pack_t pack;
    hfe_pack_init(&pack, buf, DISK_TRACKS_BYTES);
    hfe_stream_init(&l->stream, size, region, &pack);
    l->crc = 0;
    esp_err_t err = ESP_OK;
    if (from_copy) {
        for (uint32_t off = 0; off < size && err == ESP_OK; off += sizeof(chunk)) {
            uint32_t n = size - off < sizeof(chunk) ? size - off : sizeof(chunk);
            err = disk_copy_read(off, chunk, n);
            if (err == ESP_OK && !hfe_feed(l, chunk, n)) {
                err = ESP_FAIL;
            }
        }
        if (err == ESP_OK && l->crc != ci.crc) {
            err = ESP_ERR_INVALID_CRC;
        }
    } else {
        /* Piece by piece: the card is free for others in between. */
        sd_reader_t *rd = NULL;
        uint32_t got = 0, n = 0;
        sd_result_t r = sd_card_reader_open(info->sd_path, size, &rd, &n);
        if (r != SD_OK || n != size) {
            err = r == SD_NO_CARD || r == SD_IO ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
        for (uint32_t off = 0; err == ESP_OK && off < size; off += got) {
            uint32_t want = size - off < sizeof(chunk) ? size - off : sizeof(chunk);
            if (sd_card_reader_read(rd, chunk, want, &got) != SD_OK) {
                err = ESP_ERR_NOT_FOUND;        /* card taken out */
            } else if (!hfe_feed(l, chunk, got)) {
                err = ESP_FAIL;
            }
        }
        sd_card_reader_close(rd);
    }
    hfe_result_t hr = hfe_stream_end(&l->stream);
    const hfe_info_t *hi = hfe_stream_info(&l->stream);
    if (err == ESP_OK && hr != HFE_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        info->size = size;
        info->crc32 = l->crc;
        info->cylinders = hi->cylinders;
        info->heads = hi->sides;
        info->sectors = 0;
        info->hfe = true;
        info->sd_copy = from_copy;
        prepared_kind = DISK_TRACKS_HFE;
        prepared_cells = MFM_TRACK_CELLS;   /* what the ST encoder would read: in bounds */
        printf("HFE tracks: %s, %lu KiB of track buffer, %lld ms%s\n", hi->detail,
               (unsigned long)(hi->bytes_needed / 1024), (esp_timer_get_time() - t0) / 1000,
               from_copy ? " (from the copy in the flash)" : "");
    } else {
        printf("ERROR: HFE %s: %s (%s)\n", info->sd_path, esp_err_to_name(err),
               hr != HFE_OK ? hi->detail : "data");
    }
    free(region);
    free(l);
    return err;
}

/* ---- Start-up disk -------------------------------------------------------------- */

/* The start-up .ST from the SD card. ESP_ERR_NOT_FOUND: no card or the
 * card cannot be read (the copy may help); ESP_FAIL: not usable. */
static esp_err_t load_boot_sd(const char *path, uint8_t **raw, disk_info_t *info)
{
    uint8_t *buf;
    uint32_t size;
    st_info_t st;
    int64_t t0 = esp_timer_get_time();

    sd_result_t r = sd_card_load(path, ST_MAX_SIZE, &buf, &size);
    if (r != SD_OK) {
        printf("Last disk: %s on the SD card: %s\n", path, sd_result_name(r));
        /* A card without the file means it was deleted: then no copy either. */
        return r == SD_NO_CARD || r == SD_IO ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (st_check_image(buf, size, &st) != ST_OK) {
        printf("Last disk: %s on the SD card: %s\n", path, st.detail);
        free(buf);
        return ESP_FAIL;
    }
    info->size = size;
    info->crc32 = esp_rom_crc32_le(0, buf, size);
    printf("Last disk: %s (SD card, %lu bytes, CRC32 %08lx, read in %lld ms)\n", path,
           (unsigned long)size, (unsigned long)info->crc32, (esp_timer_get_time() - t0) / 1000);
    *raw = buf;
    return ESP_OK;
}

/* The start-up .ST from its copy in the flash. */
static esp_err_t load_boot_copy(uint8_t **raw, disk_info_t *info)
{
    disk_copy_info_t ci;

    if (!disk_copy_get(&ci) || ci.hfe) {
        return ESP_FAIL;
    }
    uint8_t *buf = heap_caps_malloc(ci.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    if (disk_copy_read(0, buf, ci.size) != ESP_OK || esp_rom_crc32_le(0, buf, ci.size) != ci.crc) {
        printf("Last disk: its copy in the flash cannot be read\n");
        free(buf);
        return ESP_FAIL;
    }
    info->size = ci.size;
    info->crc32 = ci.crc;
    info->sd_copy = true;
    *raw = buf;
    return ESP_OK;
}

/* Does the flash copy belong to the remembered disk? */
static bool copy_matches(const settings_t *cfg)
{
    disk_copy_info_t ci;
    return disk_copy_get(&ci) && strcmp(ci.path, cfg->last_sd) == 0 && ci.crc == cfg->last_sd_crc;
}

/*
 * Start-up disk: the one active at power-off (settings last_sd), from the
 * SD card; without the card (or when it cannot be read) from its copy in
 * the flash. An HFE disk is built into buf at once (*hfe true); an .ST
 * comes back as *raw. Nothing remembered: no disk.
 */
static esp_err_t load_boot(uint8_t *buf, uint8_t **raw, disk_info_t *info, bool *hfe)
{
    settings_t cfg;

    *raw = NULL;
    *hfe = false;
    if (ext_flash_init() != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    erase_old_library();
    settings_get(&cfg);
    if (!cfg.last_sd[0]) {
        printf("No disk remembered - insert one from the SD card through the web page.\n");
        return ESP_ERR_NOT_FOUND;
    }
    *info = (disk_info_t) { .source = DISK_SRC_SD };
    const char *name = strrchr(cfg.last_sd, '/');
    disk_title_from_filename(name ? name + 1 : cfg.last_sd, info->name);
    snprintf(info->sd_path, sizeof(info->sd_path), "%s", cfg.last_sd);

    esp_err_t err;
    if (is_hfe_name(cfg.last_sd)) {
        err = build_hfe_from(buf, info, false);
        if (err == ESP_ERR_NOT_FOUND && copy_matches(&cfg)) {
            printf("Last disk: %s from its copy in the flash (SD card not available)\n", cfg.last_sd);
            err = build_hfe_from(buf, info, true);
        }
        *hfe = err == ESP_OK;
        return err;
    }
    err = load_boot_sd(cfg.last_sd, raw, info);
    if (err == ESP_ERR_NOT_FOUND && copy_matches(&cfg)) {
        err = load_boot_copy(raw, info);
        if (err == ESP_OK) {
            printf("Last disk: %s from its copy in the flash (SD card not available)\n", cfg.last_sd);
        }
    }
    return err;
}

/* Runs under the drive lock: buffer and track length change together. */
static void set_active_buffer(void *arg)
{
    active_buf = *(int *)arg;
    disk_tracks = track_buf[active_buf];
    disk_track_cells = prepared_cells;
    disk_tracks_kind = prepared_kind;
}

esp_err_t disk_image_init(void)
{
    raw_lock = xSemaphoreCreateMutex();
    printf("PSRAM free: %u KiB before the track buffers\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    for (int i = 0; i < 2; i++) {
        track_buf[i] = heap_caps_malloc(DISK_TRACKS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!track_buf[i]) {
            printf("ERROR: no PSRAM for track buffer %c\n", 'A' + i);
            return ESP_ERR_NO_MEM;
        }
    }
    printf("PSRAM: 2 track buffers of %u KiB, %u KiB free\n",
           (unsigned)(DISK_TRACKS_BYTES / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    uint8_t *raw = NULL;
    disk_info_t info = { .source = DISK_SRC_NONE };
    st_info_t st = { 0 };
    bool hfe = false;
    if (load_boot(track_buf[0], &raw, &info, &hfe) == ESP_OK && raw &&
        st_check_image(raw, info.size, &st) != ST_OK) {
        printf("ERROR: start-up image not supported: %s\n", st.detail);
        free(raw);
        raw = NULL;
    }
    if (hfe) {
        int idx = 0;
        set_active_buffer(&idx);
        drive_swap_media(set_active_buffer, &idx, true, false);    /* not armed yet: always done */
        active_geo = info;
        disk_media_gen++;
        set_current(&info);
        return ESP_OK;                  /* flux mode: see floppy_app (after flux_stream_init) */
    }
    if (!raw) {
        info = (disk_info_t) { .source = DISK_SRC_NONE };
        strcpy(info.name, "(no disk)");
        printf("No disk inserted - insert one through the web page or the API.\n");
    }
    info.cylinders = raw ? st.cylinders : 0;
    info.heads = raw ? st.heads : 0;
    info.sectors = raw ? st.sectors : 0;

    prepared_kind = DISK_TRACKS_MFM;
    printf("Generating MFM tracks...\n");
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = build_tracks(track_buf[0], raw, &info, &prepared_cells);
    if (err != ESP_OK) {
        free(raw);
        return err;
    }
    printf("MFM tracks: %d generated in %lld ms\n", DISK_MAX_CYLS * DISK_MAX_HEADS,
           (esp_timer_get_time() - t0) / 1000);

    int idx = 0;
    set_active_buffer(&idx);
    drive_swap_media(set_active_buffer, &idx, raw != NULL, false);    /* not armed yet: always done */
    active_geo = info;
    disk_media_gen++;
    set_current(&info);

    if (raw) {
        verify_start(raw, &info);
        if (info.keep_sectors) {
            active_raw = raw;           /* kept: the start-up disk may be written */
        } else {
            free(raw);                  /* read-only: the tracks are enough */
        }
    }
    return ESP_OK;
}

esp_err_t disk_prepare(const uint8_t *raw, const disk_info_t *info)
{
    /* Writable disks: keep a copy of the sectors, so the disk can be
     * written (none for now). */
    xSemaphoreTake(raw_lock, portMAX_DELAY);
    free(prepared_raw);
    prepared_raw = NULL;
    if (raw && info->keep_sectors) {
        prepared_raw = heap_caps_malloc(info->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (prepared_raw) {
            memcpy(prepared_raw, raw, info->size);
        } else {
            printf("Disk: no PSRAM to keep the sectors - this disk cannot be written\n");
        }
    }
    xSemaphoreGive(raw_lock);
    /* The verifier may be reading the buffer that is about to be reused. */
    verify_stop();
    prepared_kind = DISK_TRACKS_MFM;
    return build_tracks(track_buf[active_buf ^ 1], raw, info, &prepared_cells);
}

esp_err_t disk_prepare_hfe(disk_info_t *info)
{
    xSemaphoreTake(raw_lock, portMAX_DELAY);
    free(prepared_raw);                 /* HFE is never written: no sectors kept */
    prepared_raw = NULL;
    xSemaphoreGive(raw_lock);
    verify_stop();
    return build_hfe_from(track_buf[active_buf ^ 1], info, false);
}

void disk_verify_active(const uint8_t *raw, const disk_info_t *info)
{
    if (raw && !info->hfe) {
        verify_start(raw, info);        /* MFM tracks only */
    }
}

esp_err_t disk_activate_prepared(const disk_info_t *info, uint32_t timeout_ms)
{
    int idx = active_buf ^ 1;
    int64_t now = esp_timer_get_time();
    int64_t selected_from = now + DISK_SWAP_SELECTED_AFTER_MS * 1000LL;
    int64_t deadline = now + timeout_ms * 1000LL;
    drive_swap_t how;

    while ((how = drive_swap_media(set_active_buffer, &idx, true,
                                   esp_timer_get_time() >= selected_from)) == DRIVE_SWAP_REFUSED) {
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* The kept sectors follow the tracks (the caller saved any changes). */
    xSemaphoreTake(raw_lock, portMAX_DELAY);
    uint8_t *old = active_raw;
    active_raw = prepared_raw;
    prepared_raw = NULL;
    active_geo = *info;
    dirty_mask = 0;
    disk_media_gen++;
    xSemaphoreGive(raw_lock);
    free(old);
    set_current(info);
    /* ST <-> HFE: the flux stream follows (restarts in the other mode). */
    flux_stream_set_mode(info->hfe ? FLUX_MODE_HFE : FLUX_MODE_ST);
    /* Let a flux-encoder refill that started on the old buffer finish
     * before anybody may overwrite that buffer. */
    vTaskDelay(pdMS_TO_TICKS(20));
    printf("Active disk: %s (%s%s)%s\n", info->name,
           info->source == DISK_SRC_PSRAM ? "PSRAM" : info->sd_copy ? "flash copy" : "SD card",
           info->hfe ? ", HFE" : "",
           how == DRIVE_SWAP_SELECTED ? " - changed while the drive was selected" : "");
    return ESP_OK;
}

/* ---- Writing ---------------------------------------------------------------- */

bool disk_writable_data(void)
{
    return active_raw != NULL && active_geo.keep_sectors;
}

esp_err_t disk_write_sector(uint32_t gen, int cyl, int head, int sector, const uint8_t *data)
{
    static uint8_t *work;
    esp_err_t err = ESP_OK;

    if (!work) {
        work = heap_caps_malloc(MFM_MAX_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!work) {
            return ESP_ERR_NO_MEM;
        }
    }
    verify_stop();                      /* it compares against the old sectors */
    xSemaphoreTake(raw_lock, portMAX_DELAY);
    const disk_info_t *g = &active_geo;
    if (gen != disk_media_gen || !active_raw) {
        err = ESP_ERR_INVALID_STATE;    /* the disk changed meanwhile */
    } else if (cyl < 0 || cyl >= g->cylinders || head < 0 || head >= g->heads ||
               sector < 1 || sector > g->sectors) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        size_t off = ((size_t)(cyl * g->heads + head) * g->sectors + (sector - 1)) * MFM_SECTOR_SIZE;
        memcpy(active_raw + off, data, MFM_SECTOR_SIZE);
        dirty_mask |= 1u << (off / DIRTY_REGION);
        if ((off + MFM_SECTOR_SIZE - 1) / DIRTY_REGION != off / DIRTY_REGION) {
            dirty_mask |= 1u << ((off + MFM_SECTOR_SIZE - 1) / DIRTY_REGION);
        }
        /* The same encoder as for reading: the track now carries the new
         * sector. Only that sector's bytes differ from what is playing. */
        mfm_layout_t layout = mfm_layout(g->sectors);
        mfm_build_track(work, &layout, sector_data(active_raw, g, cyl, head), cyl, head);
        memcpy(track_ptr(track_buf[active_buf], cyl, head), work, layout.cells / 8);
    }
    xSemaphoreGive(raw_lock);
    return err;
}

bool disk_dirty(void)
{
    return dirty_mask != 0;
}

esp_err_t disk_snapshot(disk_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    xSemaphoreTake(raw_lock, portMAX_DELAY);
    if (!dirty_mask || !active_raw) {
        xSemaphoreGive(raw_lock);
        return ESP_ERR_NOT_FOUND;
    }
    snap->data = heap_caps_malloc(active_geo.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap->data) {
        xSemaphoreGive(raw_lock);
        return ESP_ERR_NO_MEM;
    }
    memcpy(snap->data, active_raw, active_geo.size);
    snap->size = active_geo.size;
    snap->mask = dirty_mask;
    snap->gen = disk_media_gen;
    dirty_mask = 0;
    xSemaphoreGive(raw_lock);
    return ESP_OK;
}

void disk_snapshot_failed(const disk_snapshot_t *snap)
{
    xSemaphoreTake(raw_lock, portMAX_DELAY);
    if (snap->gen == disk_media_gen) {
        dirty_mask |= snap->mask;       /* still to be saved */
    }
    xSemaphoreGive(raw_lock);
}
