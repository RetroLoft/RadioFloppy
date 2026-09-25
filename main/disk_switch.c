/*
 * Changing the active disk. See disk_switch.h.
 *
 * Every switch: image in PSRAM -> validated -> encoded into the inactive
 * track buffer (disk_prepare) -> swapped in while our drive is not selected
 * (disk_activate_prepared) -> tracks verified again in the background.
 * Runs in task context on core 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "disk_switch.h"
#include "image_store.h"
#include "st_image.h"

#define ACTIVATE_TIMEOUT_MS 3000    /* wait for our drive to be deselected */

static SemaphoreHandle_t lock;
static uint8_t *psram_raw;          /* the kept PSRAM image */
static disk_info_t psram_info;

static esp_err_t fail(switch_error_t *e, const char *code, const char *fmt, const char *arg)
{
    e->code = code;
    snprintf(e->msg, sizeof(e->msg), fmt, arg);
    return ESP_FAIL;
}

static int64_t load_us;            /* time spent reading the image, for the log */

static esp_err_t activate_locked(const uint8_t *raw, const disk_info_t *info, switch_error_t *e)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = disk_prepare(raw, info);
    if (err != ESP_OK) {
        return fail(e, "INSUFFICIENT_MEMORY", "no memory to encode the tracks%s", "");
    }
    int64_t t1 = esp_timer_get_time();
    if (disk_activate_prepared(info, ACTIVATE_TIMEOUT_MS) == ESP_ERR_TIMEOUT) {
        e->code = "DRIVE_BUSY";
        snprintf(e->msg, sizeof(e->msg), "drive stayed selected for %d ms; disk not changed",
                 ACTIVATE_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    int64_t t2 = esp_timer_get_time();
    printf("Disk switch: load %lld ms, encode %lld ms, wait for drive %lld ms\n",
           load_us / 1000, (t1 - t0) / 1000, (t2 - t1) / 1000);
    load_us = 0;
    disk_verify_active(raw, info);      /* background, log only */
    return ESP_OK;
}

static esp_err_t image_locked(uint16_t id, switch_error_t *e)
{
    char name[12];
    image_record_t r;
    snprintf(name, sizeof(name), "%u", id);

    if (!image_store_get(id, &r) || !image_store_is_valid(&r)) {
        return fail(e, "IMAGE_NOT_FOUND", "image %s does not exist or holds no valid data", name);
    }
    disk_info_t info = { .source = DISK_SRC_FLASH, .image_id = id,
                         .size = r.original_size, .crc32 = r.crc32 };
    image_store_title(&r, info.name);

    uint8_t *raw = heap_caps_malloc(r.original_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {
        return fail(e, "INSUFFICIENT_MEMORY", "no PSRAM to load image %s", name);
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = image_store_load(id, raw);      /* whole image, CRC checked */
    load_us = esp_timer_get_time() - t0;
    st_info_t st;
    if (err != ESP_OK) {
        fail(e, "FLASH_ERROR", "reading image %s failed", name);
    } else if (st_check_image(raw, r.original_size, &st) != ST_OK) {
        err = ESP_FAIL;
        e->code = "UNSUPPORTED_GEOMETRY";
        snprintf(e->msg, sizeof(e->msg), "%s", st.detail);
    } else {
        info.cylinders = st.cylinders;
        info.heads = st.heads;
        info.sectors = st.sectors;
        err = activate_locked(raw, &info, e);
    }
    free(raw);
    return err;
}

static esp_err_t psram_locked(switch_error_t *e)
{
    if (!psram_raw) {
        return fail(e, "IMAGE_NOT_FOUND", "no PSRAM image%s", "");
    }
    return activate_locked(psram_raw, &psram_info, e);
}

void disk_switch_init(void)
{
    lock = xSemaphoreCreateMutex();
}

esp_err_t disk_switch_image(uint16_t image_id, switch_error_t *e)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = image_locked(image_id, e);
    xSemaphoreGive(lock);
    return err;
}

esp_err_t disk_switch_raw(const uint8_t *raw, const disk_info_t *info, switch_error_t *e)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = activate_locked(raw, info, e);
    xSemaphoreGive(lock);
    return err;
}

esp_err_t disk_switch_psram_upload(uint8_t *raw, const disk_info_t *info, switch_error_t *e)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = activate_locked(raw, info, e);
    if (err == ESP_OK) {
        free(psram_raw);            /* the previous PSRAM image */
        psram_raw = raw;
        psram_info = *info;
    }
    xSemaphoreGive(lock);
    return err;
}

bool disk_switch_psram_info(disk_info_t *info)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool have = psram_raw != NULL;
    if (have) {
        *info = psram_info;
    }
    xSemaphoreGive(lock);
    return have;
}

esp_err_t disk_switch_step(int dir, switch_error_t *e)
{
    /* List entries: 0 = PSRAM image, else a library image id. */
    static image_record_t *recs;        /* 24 KiB: in PSRAM, not internal RAM */
    static uint16_t list[IMG_MAX_RECORDS + 1];
    int n = 0;

    if (!recs) {
        recs = heap_caps_malloc(IMG_MAX_RECORDS * sizeof(*recs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!recs) {
            return fail(e, "INSUFFICIENT_MEMORY", "no memory for the image list%s", "");
        }
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (psram_raw) {
        list[n++] = 0;
    }
    int count = image_store_list(recs, IMG_MAX_RECORDS);
    for (int i = 0; i < count; i++) {
        if (image_store_is_valid(&recs[i])) {
            list[n++] = recs[i].id;
        }
    }
    if (n == 0) {
        xSemaphoreGive(lock);
        return fail(e, "IMAGE_NOT_FOUND", "no images to choose from%s", "");
    }

    disk_info_t cur;
    disk_get_current(&cur);
    int pos = -1;
    for (int i = 0; i < n; i++) {
        if ((list[i] == 0 && cur.source == DISK_SRC_PSRAM) ||
            (list[i] != 0 && cur.source == DISK_SRC_FLASH && cur.image_id == list[i])) {
            pos = i;
        }
    }
    /* Unknown current disk: right starts at the first, left at the last. */
    int next = pos < 0 ? (dir > 0 ? 0 : n - 1) : (pos + dir + n) % n;
    if (next == pos) {
        xSemaphoreGive(lock);
        fail(e, "SAME_DISK", "only one disk to choose from%s", "");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = list[next] == 0 ? psram_locked(e) : image_locked(list[next], e);
    xSemaphoreGive(lock);
    return err;
}
