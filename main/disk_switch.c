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
#include "slot_store.h"
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
    esp_err_t err = disk_prepare(raw, info->heads);
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
    disk_verify_active(raw, info->size, info->heads);      /* background, log only */
    return ESP_OK;
}

static esp_err_t slot_locked(int slot, switch_error_t *e)
{
    char name[12];
    snprintf(name, sizeof(name), "%d", slot + 1);

    if (!slot_store_is_valid(slot)) {
        return fail(e, "SLOT_EMPTY", "slot %s holds no valid image", name);
    }
    const slot_record_t *r = slot_store_record(slot);
    disk_info_t info = { .source = DISK_SRC_FLASH, .slot = slot,
                         .size = r->size, .crc32 = r->crc32 };
    memcpy(info.name, r->name, sizeof(info.name));

    uint8_t *raw = heap_caps_malloc(r->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {
        return fail(e, "INSUFFICIENT_MEMORY", "no PSRAM to load slot %s", name);
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = slot_store_load(slot, raw);     /* checks the CRC */
    load_us = esp_timer_get_time() - t0;
    st_info_t st;
    if (err != ESP_OK) {
        fail(e, "FLASH_ERROR", "reading slot %s failed", name);
    } else if (st_check_image(raw, r->size, &st) != ST_OK) {
        err = ESP_FAIL;
        e->code = "UNSUPPORTED_GEOMETRY";
        snprintf(e->msg, sizeof(e->msg), "%s", st.detail);
    } else {
        info.heads = st.heads;
        err = activate_locked(raw, &info, e);
    }
    free(raw);
    return err;
}

static esp_err_t psram_locked(switch_error_t *e)
{
    if (!psram_raw) {
        return fail(e, "SLOT_EMPTY", "no PSRAM image%s", "");
    }
    return activate_locked(psram_raw, &psram_info, e);
}

void disk_switch_init(void)
{
    lock = xSemaphoreCreateMutex();
}

esp_err_t disk_switch_slot(int slot, switch_error_t *e)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = slot_locked(slot, e);
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
    /* List entries: -1 = PSRAM image, 0..19 = slot. */
    int list[RF_SLOT_COUNT + 1];
    int n = 0;

    xSemaphoreTake(lock, portMAX_DELAY);
    if (psram_raw) {
        list[n++] = -1;
    }
    for (int i = 0; i < RF_SLOT_COUNT; i++) {
        if (slot_store_is_valid(i)) {
            list[n++] = i;
        }
    }
    if (n == 0) {
        xSemaphoreGive(lock);
        return fail(e, "SLOT_EMPTY", "no images to choose from%s", "");
    }

    disk_info_t cur;
    disk_get_current(&cur);
    int pos = -1;
    for (int i = 0; i < n; i++) {
        if ((list[i] == -1 && cur.source == DISK_SRC_PSRAM) ||
            (list[i] >= 0 && cur.source == DISK_SRC_FLASH && cur.slot == list[i])) {
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

    esp_err_t err = list[next] < 0 ? psram_locked(e) : slot_locked(list[next], e);
    xSemaphoreGive(lock);
    return err;
}
