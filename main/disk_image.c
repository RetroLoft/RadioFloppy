/*
 * The active floppy. See disk_image.h.
 *
 * Images are raw .ST sector dumps (80 cylinders, 9 x 512 bytes, 1 or 2
 * sides), from the external flash slot store or a temporary upload. Every
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
#include "legacy_catalog.h"
#include "slot_store.h"

uint8_t *volatile disk_tracks;

static uint8_t *track_buf[2];       /* A/B track buffers in PSRAM */
static int active_buf;              /* index of the buffer in use */
static disk_info_t current = { .source = DISK_SRC_NONE, .slot = -1 };
static portMUX_TYPE info_lock = portMUX_INITIALIZER_UNLOCKED;

/* .ST order: cylinder, then head, then sectors 1..9. */
static const uint8_t *sector_data(const uint8_t *raw, int heads, int cyl, int head)
{
    return raw + (size_t)(cyl * heads + head) * MFM_SECTORS * MFM_SECTOR_SIZE;
}

/* Encode all tracks of raw into buf (side 1 unformatted for 1 head). */
static esp_err_t build_tracks(uint8_t *buf, const uint8_t *raw, int heads)
{
    static uint8_t *blank;          /* unformatted track, built once */
    uint8_t *work = heap_caps_malloc(MFM_TRACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!blank) {
        blank = heap_caps_malloc(MFM_TRACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (blank) {
            mfm_build_blank_track(blank);
        }
    }
    if (!work || !blank) {
        free(work);
        return ESP_ERR_NO_MEM;
    }
    for (int cyl = 0; cyl < DISK_CYLINDERS; cyl++) {
        for (int head = 0; head < DISK_MAX_HEADS; head++) {
            const uint8_t *src = blank;
            if (raw && head < heads) {
                mfm_build_track(work, sector_data(raw, heads, cyl, head), cyl, head);
                src = work;
            }
            memcpy(buf + (size_t)(cyl * DISK_MAX_HEADS + head) * MFM_TRACK_BYTES,
                   src, MFM_TRACK_BYTES);
        }
    }
    free(work);
    return ESP_OK;
}

/*
 * Background check of the active disk: decode every track again and
 * compare with the image. Runs after a switch on its own copy of the
 * image; a new switch aborts it instead of waiting for it.
 */
static struct {
    uint8_t *raw;               /* own copy, freed by the task */
    int heads;
    const uint8_t *tracks;
} verify_job;
static volatile bool verify_running;
static volatile bool verify_abort;

static void verify_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    int bad = 0, done = 0;

    for (int cyl = 0; cyl < DISK_CYLINDERS && !verify_abort; cyl++) {
        for (int head = 0; head < verify_job.heads; head++) {
            const uint8_t *t = verify_job.tracks + (size_t)(cyl * DISK_MAX_HEADS + head) * MFM_TRACK_BYTES;
            if (mfm_verify_track(t, sector_data(verify_job.raw, verify_job.heads, cyl, head),
                                 cyl, head) != MFM_SECTORS) {
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
static void verify_start(const uint8_t *raw, uint32_t size, int heads)
{
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
    verify_job.heads = heads;
    verify_job.tracks = track_buf[active_buf];
    verify_running = true;
    /* Lowest priority, on the core that does not run the GPIO/RMT ISRs. */
    xTaskCreatePinnedToCore(verify_task, "mfm_verify", 4096, NULL, 1, NULL, 1);
}

static void set_current(const disk_info_t *info)
{
    portENTER_CRITICAL(&info_lock);
    current = *info;
    portEXIT_CRITICAL(&info_lock);
}

void disk_get_current(disk_info_t *info)
{
    portENTER_CRITICAL(&info_lock);
    *info = current;
    portEXIT_CRITICAL(&info_lock);
}

void disk_note_slot_changed(int slot)
{
    portENTER_CRITICAL(&info_lock);
    if (current.source == DISK_SRC_FLASH && current.slot == slot) {
        current.slot_changed = true;
    }
    portEXIT_CRITICAL(&info_lock);
}

/* Log the slot catalog and any legacy v1 catalog. */
static void report_store(slot_store_state_t st)
{
    switch (st) {
    case SLOT_STORE_VALID: {
        int used = 0;
        for (int i = 0; i < RF_SLOT_COUNT; i++) {
            used += slot_store_is_valid(i);
        }
        printf("Slot catalog: OK (generation %lu, %d of %d slots in use)\n",
               (unsigned long)slot_store_generation(), used, RF_SLOT_COUNT);
        for (int i = 0; i < RF_SLOT_COUNT; i++) {
            const slot_record_t *r = slot_store_record(i);
            if (r->status == SLOT_EMPTY) {
                continue;
            }
            printf("  Slot %2d: %-9s %-24.*s %7lu bytes\n", i + 1,
                   r->status == SLOT_VALID ? (slot_store_is_valid(i) ? "valid" : "BAD")
                   : r->status == SLOT_BUILDING ? "building"
                   : r->status == SLOT_DELETED ? "deleted" : "unknown",
                   SLOT_NAME_LEN, r->name, (unsigned long)r->size);
        }
        break;
    }
    case SLOT_STORE_BLANK:
        printf("Slot catalog: empty (RadioFloppy slot storage not initialised)\n");
        break;
    default:
        printf("Slot catalog: INVALID or unknown data in the catalog sectors - not touched\n");
        break;
    }

    if (legacy_catalog_open()) {
        printf("Legacy v1 catalog found (generation %lu):\n",
               (unsigned long)legacy_catalog_generation());
        for (int i = 0; i < LEGACY_RECORDS; i++) {
            const legacy_record_t *l = legacy_catalog_record(i);
            if (l->status != LEGACY_ST_VALID) {
                continue;
            }
            int slot = -1;
            for (int s = 0; s < RF_SLOT_COUNT; s++) {
                if (l->start == rf_slot_start(s)) {
                    slot = s;
                }
            }
            printf("  \"%.*s\" %lu bytes at 0x%06lx", LEGACY_NAME_LEN, l->name,
                   (unsigned long)l->size, (unsigned long)l->start);
            if (slot >= 0 && l->size <= RF_IMAGE_MAX_SIZE) {
                printf(" = start of slot %d (migratable without moving data)\n", slot + 1);
            } else {
                printf(" (not slot aligned: would have to be copied)\n");
            }
        }
    }
}

/* Read size bytes from addr into a new PSRAM buffer and check the CRC. */
static esp_err_t read_image(uint32_t addr, uint32_t size, uint32_t crc,
                            const uint8_t **data)
{
    if (size == 0 || size > RF_IMAGE_MAX_SIZE) {
        printf("ERROR: registered image size %lu is invalid\n", (unsigned long)size);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ext_flash_read(addr, buf, size);
    if (err == ESP_OK && slot_store_crc32(0, buf, size) != crc) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) {
        printf("ERROR: reading the image failed: %s\n",
               err == ESP_ERR_INVALID_CRC ? "CRC mismatch" : esp_err_to_name(err));
        free(buf);
        return err;
    }
    printf("Image size: %lu bytes\n", (unsigned long)size);
    printf("Image CRC: OK (%08lx, read in %lld ms)\n", (unsigned long)crc,
           (esp_timer_get_time() - t0) / 1000);
    *data = buf;
    return ESP_OK;
}

/* Find the start-up image: by name, else the first valid slot, else legacy. */
static esp_err_t load_boot_image(uint8_t **raw, disk_info_t *info)
{
    esp_err_t err = ext_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    slot_store_state_t st = slot_store_open();
    report_store(st);

#if DISK_MIGRATE_LEGACY
    if (st == SLOT_STORE_BLANK && legacy_catalog_open()) {
        int n = 0;
        err = slot_store_migrate_legacy(&n);
        printf("Legacy migration: %s, %d image(s) taken over into the slot catalog "
               "(no image data moved)\n", err == ESP_OK ? "OK" : esp_err_to_name(err), n);
        report_store(slot_store_state());
    }
#endif

    int slot = slot_store_find_name(DISK_BOOT_IMAGE);
    for (int i = 0; slot < 0 && i < RF_SLOT_COUNT; i++) {
        if (slot_store_is_valid(i)) {
            slot = i;
        }
    }
    const uint8_t *data = NULL;
    if (slot >= 0) {
        const slot_record_t *r = slot_store_record(slot);
        printf("Selected image: %s\n", r->name);
        printf("Source: EXTERNAL SPI FLASH, slot %d (0x%06lx)\n", slot + 1,
               (unsigned long)rf_slot_start(slot));
        err = read_image(rf_slot_start(slot), r->size, r->crc32, &data);
        if (err == ESP_OK) {
            *info = (disk_info_t) { .source = DISK_SRC_FLASH, .slot = slot,
                                    .size = r->size, .crc32 = r->crc32 };
            memcpy(info->name, r->name, sizeof(info->name) - 1);
        }
    } else if (legacy_catalog_open()) {
        err = ESP_ERR_NOT_FOUND;
        for (int i = 0; i < LEGACY_RECORDS; i++) {
            const legacy_record_t *l = legacy_catalog_record(i);
            if (l->status != LEGACY_ST_VALID ||
                strncmp(l->name, DISK_BOOT_IMAGE, LEGACY_NAME_LEN) != 0) {
                continue;
            }
            printf("Selected image: %.*s\n", LEGACY_NAME_LEN, l->name);
            printf("Source: EXTERNAL SPI FLASH, legacy v1 catalog (0x%06lx) - "
                   "MIGRATION TO SLOTS PENDING\n", (unsigned long)l->start);
            err = read_image(l->start, l->size, l->crc32, &data);
            if (err == ESP_OK) {
                *info = (disk_info_t) { .source = DISK_SRC_FLASH, .slot = -1,
                                        .size = l->size, .crc32 = l->crc32 };
                memcpy(info->name, l->name, sizeof(info->name) - 1);
            }
            break;
        }
    } else {
        printf("No image on the external flash.\n");
        err = ESP_ERR_NOT_FOUND;
    }
    *raw = (uint8_t *)data;
    return err;
}

static void set_active_buffer(void *arg)
{
    active_buf = *(int *)arg;
    disk_tracks = track_buf[active_buf];
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
    disk_info_t info = { .source = DISK_SRC_NONE, .slot = -1 };
    st_info_t st = { 0 };
    if (load_boot_image(&raw, &info) == ESP_OK &&
        st_check_image(raw, info.size, &st) != ST_OK) {
        printf("ERROR: start-up image not supported: %s\n", st.detail);
        free(raw);
        raw = NULL;
    }
    if (!raw) {
        info = (disk_info_t) { .source = DISK_SRC_NONE, .slot = -1 };
        strcpy(info.name, "(no disk)");
        printf("No disk inserted - upload or activate one through the API.\n");
    }
    info.heads = raw ? st.heads : 0;

    printf("Generating MFM tracks...\n");
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = build_tracks(track_buf[0], raw, info.heads);
    if (err != ESP_OK) {
        free(raw);
        return err;
    }
    printf("MFM tracks: %d generated in %lld ms\n", DISK_CYLINDERS * DISK_MAX_HEADS,
           (esp_timer_get_time() - t0) / 1000);

    int idx = 0;
    set_active_buffer(&idx);
    drive_swap_media(set_active_buffer, &idx, raw != NULL);   /* not armed yet: always done */
    set_current(&info);

    if (raw) {
        verify_start(raw, info.size, info.heads);
        free(raw);
    }
    return ESP_OK;
}

esp_err_t disk_prepare(const uint8_t *raw, int heads)
{
    /* The verifier may be reading the buffer that is about to be reused. */
    verify_stop();
    return build_tracks(track_buf[active_buf ^ 1], raw, heads);
}

void disk_verify_active(const uint8_t *raw, uint32_t size, int heads)
{
    verify_start(raw, size, heads);
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
           info->source == DISK_SRC_PSRAM ? "PSRAM" : "flash slot");
    return ESP_OK;
}
