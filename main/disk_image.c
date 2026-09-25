/*
 * The active floppy. See disk_image.h.
 *
 * Images are raw .ST sector dumps (79-84 cylinders, 9-11 x 512 bytes, 1 or
 * 2 sides), from the external flash image library (read completely into
 * PSRAM through the image reader) or a temporary upload. Every
 * track is encoded once into a PSRAM track buffer (80 x 2 x 12500 bytes),
 * so a STEP or SIDE change never waits for encoding; side 1 of a
 * single-sided image is an unformatted track. New images go into the
 * inactive buffer and are verified before the buffers are swapped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "disk_image.h"
#include "drive_emu.h"
#include "st_image.h"
#include "ext_flash.h"
#include "image_store.h"
#include "settings.h"

uint8_t *volatile disk_tracks;
volatile uint32_t disk_track_cells = MFM_TRACK_CELLS;

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
        printf("MFM verify: aborted by a disk change\n");
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

void disk_note_image_changed(uint16_t image_id)
{
    portENTER_CRITICAL(&info_lock);
    if (current.source == DISK_SRC_FLASH && current.image_id == image_id) {
        current.image_changed = true;
    }
    portEXIT_CRITICAL(&info_lock);
}

/* Log the image library. */
static void report_store(store_state_t st)
{
    const rf_geometry_t *g = image_store_geometry();

    switch (st) {
    case STORE_VALID: {
        store_usage_t u;
        image_store_usage(&u);
        printf("Image library: OK (generation %lu), %u KiB blocks, %u of %u blocks used (%u%%), "
               "%u image(s)\n", (unsigned long)image_store_generation(),
               (unsigned)(g->block_size / 1024), u.blocks_used, u.blocks_total, u.used_percent,
               u.images);
        image_record_t *list = heap_caps_malloc(IMG_MAX_RECORDS * sizeof(*list),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int n = list ? image_store_list(list, IMG_MAX_RECORDS) : 0;
        for (int i = 0; i < n; i++) {
            char title[IMG_TITLE_SIZE];
            image_store_title(&list[i], title);
            printf("  #%-3u id %-4u %-10s %-40.40s %8lu bytes, blocks", list[i].sequence,
                   list[i].id, list[i].status == IMG_VALID ? "valid" : "incomplete", title,
                   (unsigned long)list[i].original_size);
            /* Block list as ranges, e.g. 1-6,20-26 (diagnostics only). */
            for (int b = 0; b < list[i].block_count; b++) {
                int e = b;
                while (e + 1 < list[i].block_count && list[i].blocks[e + 1] == list[i].blocks[e] + 1) {
                    e++;
                }
                printf("%s%u", b ? "," : " ", list[i].blocks[b]);
                if (e > b) {
                    printf("-%u", list[i].blocks[e]);
                }
                b = e;
            }
            printf("\n");
        }
        free(list);
        break;
    }
    case STORE_OLD_FORMAT:
        printf("Image library: external flash holds an OLD RadioFloppy format - not used.\n"
               "  Initialise it (all images are erased): POST /api/v1/storage/format\n");
        break;
    case STORE_INVALID:
        printf("Image library: INVALID or unknown data in block 0 (or other flash size) - "
               "not touched\n");
        break;
    default:
        printf("Image library: not available\n");
        break;
    }
}

/* Start-up image: the one active at power-off, else by name, else the first
 * valid image in sequence order. */
static esp_err_t load_boot_image(uint8_t **raw, disk_info_t *info)
{
    esp_err_t err = ext_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    store_state_t st = image_store_open(ext_flash_size());
    report_store(st);

    settings_t cfg;
    image_record_t last;
    settings_get(&cfg);
    uint16_t id = 0;
    if (cfg.last_image_id && image_store_get(cfg.last_image_id, &last) &&
        image_store_is_valid(&last)) {
        id = cfg.last_image_id;
        printf("Last disk: image %u (active at power-off)\n", id);
    }
    if (!id) {
        id = image_store_find_name(DISK_BOOT_IMAGE);
    }
    if (!id) {
        image_record_t *list = heap_caps_malloc(IMG_MAX_RECORDS * sizeof(*list),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int n = list ? image_store_list(list, IMG_MAX_RECORDS) : 0;
        for (int i = 0; i < n && !id; i++) {
            if (list[i].status == IMG_VALID) {
                id = list[i].id;
            }
        }
        free(list);
    }
    image_record_t r;
    if (!id || !image_store_get(id, &r)) {
        printf("No image on the external flash.\n");
        return ESP_ERR_NOT_FOUND;
    }
    char title[IMG_TITLE_SIZE];
    image_store_title(&r, title);
    printf("Selected image: %s\n", title);
    printf("Source: EXTERNAL SPI FLASH, image id %u, %u block(s)\n", r.id, r.block_count);

    uint8_t *buf = heap_caps_malloc(r.original_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    int64_t t0 = esp_timer_get_time();
    err = image_store_load(id, buf);        /* reads through the block list, checks the CRC */
    if (err != ESP_OK) {
        printf("ERROR: reading the image failed: %s\n",
               err == ESP_ERR_INVALID_CRC ? "CRC mismatch" : esp_err_to_name(err));
        free(buf);
        return err;
    }
    printf("Image size: %lu bytes\n", (unsigned long)r.original_size);
    printf("Image CRC: OK (%08lx, read in %lld ms)\n", (unsigned long)r.crc32,
           (esp_timer_get_time() - t0) / 1000);
    *info = (disk_info_t) { .source = DISK_SRC_FLASH, .image_id = id,
                            .size = r.original_size, .crc32 = r.crc32 };
    memcpy(info->name, title, sizeof(info->name));
    *raw = buf;
    return ESP_OK;
}

/* Runs under the drive lock: buffer and track length change together. */
static void set_active_buffer(void *arg)
{
    active_buf = *(int *)arg;
    disk_tracks = track_buf[active_buf];
    disk_track_cells = prepared_cells;
}

esp_err_t disk_image_init(void)
{
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
    if (load_boot_image(&raw, &info) == ESP_OK &&
        st_check_image(raw, info.size, &st) != ST_OK) {
        printf("ERROR: start-up image not supported: %s\n", st.detail);
        free(raw);
        raw = NULL;
    }
    if (!raw) {
        info = (disk_info_t) { .source = DISK_SRC_NONE };
        strcpy(info.name, "(no disk)");
        printf("No disk inserted - upload or activate one through the API.\n");
    }
    info.cylinders = raw ? st.cylinders : 0;
    info.heads = raw ? st.heads : 0;
    info.sectors = raw ? st.sectors : 0;

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
    drive_swap_media(set_active_buffer, &idx, raw != NULL);   /* not armed yet: always done */
    set_current(&info);

    if (raw) {
        verify_start(raw, &info);
        free(raw);
    }
    return ESP_OK;
}

esp_err_t disk_prepare(const uint8_t *raw, const disk_info_t *info)
{
    /* The verifier may be reading the buffer that is about to be reused. */
    verify_stop();
    return build_tracks(track_buf[active_buf ^ 1], raw, info, &prepared_cells);
}

void disk_verify_active(const uint8_t *raw, const disk_info_t *info)
{
    verify_start(raw, info);
}

esp_err_t disk_activate_prepared(const disk_info_t *info, uint32_t timeout_ms)
{
    int idx = active_buf ^ 1;
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;

    while (!drive_swap_media(set_active_buffer, &idx, true)) {
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    set_current(info);
    /* Let a flux-encoder refill that started on the old buffer finish
     * before anybody may overwrite that buffer. */
    vTaskDelay(pdMS_TO_TICKS(20));
    printf("Active disk: %s (%s)\n", info->name,
           info->source == DISK_SRC_PSRAM ? "PSRAM" : "image library");
    return ESP_OK;
}
