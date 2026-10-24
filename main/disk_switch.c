/*
 * Changing the active disk. See disk_switch.h.
 *
 * Every switch: image read (.ST into PSRAM) -> validated -> encoded into
 * the inactive track buffer (disk_prepare; HFE files are read straight into
 * it, disk_prepare_hfe) -> swapped in when no write is in the way
 * (disk_activate_prepared) -> .ST tracks verified again in the background.
 * Runs in task context on core 1.
 *
 * A disk from the SD card is remembered in the settings (restored at the
 * next start-up) and copied to the flash (disk_copy.h), but only
 * REMEMBER_MS after the last change and only if it differs from what is
 * stored: stepping through disks with the buttons writes the flash once,
 * not for every press.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

#include "disk_copy.h"
#include "disk_switch.h"
#include "disk_title.h"
#include "disk_write.h"
#include "flash_layout.h"
#include "sd_card.h"
#include "settings.h"
#include "st_image.h"

#define ACTIVATE_TIMEOUT_MS 3000    /* wait for our drive to be deselected */
#define REMEMBER_MS         5000    /* quiet time before the last disk is stored */
#define STEP_MAX            400     /* image files of one folder the buttons know */
#define SD_HFE_MAX          (8u * 1024u * 1024u)

static SemaphoreHandle_t lock;
static uint8_t *psram_raw;          /* the kept PSRAM image */
static disk_info_t psram_info;
static char browse_dir[SD_REL_MAX + 1];     /* the buttons' folder */
static bool enabled = true;         /* false: unsupported machine profile */

/* ---- Remembering the disk and its copy in the flash ---------------------------- */

static TaskHandle_t remember_task;
static struct {
    char path[SD_REL_MAX + 1];
    uint32_t crc;
    bool hfe;
} remember_sd;
static portMUX_TYPE remember_lock = portMUX_INITIALIZER_UNLOCKED;

static bool is_hfe_name(const char *path)
{
    size_t n = strlen(path);
    return n >= 4 && strcasecmp(path + n - 4, ".hfe") == 0;
}

static bool is_st_name(const char *path)
{
    size_t n = strlen(path);
    return n >= 3 && strcasecmp(path + n - 3, ".st") == 0;
}

/*
 * Keep a copy of the disk in the flash, so it also comes back at start-up
 * without the card: only when the file still is the active disk (same
 * CRC) and the flash does not hold that copy yet.
 */
static void copy_to_flash(const char *path, uint32_t crc, bool hfe)
{
    static uint8_t buf[8192];           /* only the remember task uses it */
    disk_copy_info_t ci;
    sd_reader_t *rd = NULL;
    uint32_t size, got, c = 0, done = 0;

    disk_copy_lock();                   /* not while written sectors are being saved */
    bool have = disk_copy_get(&ci);
    if (have && strcmp(ci.path, path) == 0 && ci.crc == crc) {
        disk_copy_unlock();
        return;                         /* already there */
    }
    if (have && ci.pending) {           /* kept until a start-up with the card syncs it */
        printf("Flash copy: %s not copied (the copy of %s has changes that are not on the "
               "SD card yet)\n", path, ci.path);
        disk_copy_unlock();
        return;
    }
    sd_result_t r = sd_card_reader_open(path, RF_COPY_MAX, &rd, &size);
    if (r != SD_OK) {
        disk_copy_unlock();
        printf("Flash copy: %s not copied (%s%s)\n", path, sd_result_name(r),
               r == SD_INVALID ? ", or larger than the copy area" : "");
        return;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = disk_copy_begin(size);
    /* A piece from the card (the card is free again), then into the flash. */
    while (err == ESP_OK && done < size) {
        uint32_t want = size - done < sizeof(buf) ? size - done : sizeof(buf);
        if (sd_card_reader_read(rd, buf, want, &got) != SD_OK) {
            err = ESP_FAIL;
            break;
        }
        c = esp_rom_crc32_le(c, buf, got);
        err = disk_copy_write(buf, got);
        done += got;
    }
    sd_card_reader_close(rd);
    if (err != ESP_OK) {
        printf("Flash copy: %s not copied (%s)\n", path, esp_err_to_name(err));
        disk_copy_unlock();
        return;
    }
    if (c != crc) {
        printf("Flash copy: %s changed since it was inserted - no copy\n", path);
        disk_copy_unlock();
        return;
    }
    ci = (disk_copy_info_t) { .size = size, .crc = crc, .hfe = hfe };
    snprintf(ci.path, sizeof(ci.path), "%s", path);
    err = disk_copy_commit(&ci);
    disk_copy_unlock();
    printf("Flash copy: %s %s (%lu KiB, %lld ms)\n", path, err == ESP_OK ? "kept in the flash" : "FAILED",
           (unsigned long)(size / 1024), (esp_timer_get_time() - t0) / 1000);
}

/* Waits for a change, then until REMEMBER_MS passed without another one. */
static void remember_task_fn(void *arg)
{
    uint32_t v;

    while (true) {
        xTaskNotifyWait(0, UINT32_MAX, &v, portMAX_DELAY);
        while (xTaskNotifyWait(0, UINT32_MAX, &v, pdMS_TO_TICKS(REMEMBER_MS)) == pdTRUE) {
        }
        char path[SD_REL_MAX + 1];
        portENTER_CRITICAL(&remember_lock);
        memcpy(path, remember_sd.path, sizeof(path));
        uint32_t crc = remember_sd.crc;
        bool hfe = remember_sd.hfe;
        portEXIT_CRITICAL(&remember_lock);
        settings_t s;
        settings_get(&s);
        if (strcmp(s.last_sd, path) != 0 || s.last_sd_crc != crc) {
            esp_err_t err = settings_set_last_sd(path, crc);
            printf("Last disk: %s remembered%s\n", path, err == ESP_OK ? "" : " - FAILED");
        }
        copy_to_flash(path, crc, hfe);
    }
}

/* A disk from the SD card became active: remember it (later, see REMEMBER_MS). */
static void remember(const disk_info_t *info)
{
    if (remember_task && info->source == DISK_SRC_SD && !info->sd_copy) {
        portENTER_CRITICAL(&remember_lock);
        memcpy(remember_sd.path, info->sd_path, sizeof(remember_sd.path));
        remember_sd.crc = info->crc32;
        remember_sd.hfe = info->hfe;
        portEXIT_CRITICAL(&remember_lock);
        xTaskNotify(remember_task, 1, eSetValueWithOverwrite);
    }
}

/* ---- Switching ----------------------------------------------------------------- */

static esp_err_t fail(switch_error_t *e, const char *code, const char *fmt, const char *arg)
{
    e->code = code;
    snprintf(e->msg, sizeof(e->msg), fmt, arg);
    return ESP_FAIL;
}

static int64_t load_us;            /* time spent reading the image, for the log */

static esp_err_t activate_locked(const uint8_t *raw, disk_info_t *info, switch_error_t *e)
{
    if (!enabled) {
        return fail(e, "MACHINE_NOT_SUPPORTED", "floppy emulation is off: support for the "
                    "selected computer is not available yet%s", "");
    }
    /* Written sectors of the current disk are saved before it goes. */
    if (disk_write_flush() != ESP_OK) {
        disk_write_refresh();
        return fail(e, "UNSAVED_CHANGES", "changes written to the current disk could not be "
                    "saved; the disk was not changed%s", "");
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = info->hfe ? disk_prepare_hfe(info) : disk_prepare(raw, info);
    if (err == ESP_ERR_NO_MEM) {
        return fail(e, "INSUFFICIENT_MEMORY", "no memory to prepare the tracks%s", "");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return fail(e, "SD_NOT_PRESENT", "the SD card cannot be read%s", "");
    }
    if (err != ESP_OK) {
        return fail(e, "INVALID_IMAGE", "the image is not usable (see the device log)%s", "");
    }
    int64_t t1 = esp_timer_get_time();
    if (disk_activate_prepared(info, ACTIVATE_TIMEOUT_MS) == ESP_ERR_TIMEOUT) {
        disk_write_refresh();           /* the old disk stays */
        e->code = "DRIVE_BUSY";
        snprintf(e->msg, sizeof(e->msg), "the computer kept writing to the drive for %d ms; "
                 "disk not changed", ACTIVATE_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    int64_t t2 = esp_timer_get_time();
    printf("Disk switch: load %lld ms, prepare %lld ms, wait for drive %lld ms\n",
           load_us / 1000, (t1 - t0) / 1000, (t2 - t1) / 1000);
    load_us = 0;
    disk_verify_active(raw, info);      /* background, log only */
    disk_write_refresh();               /* write protection for the new disk */
    remember(info);
    return ESP_OK;
}

static void dirname_of(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n >= size) {
        n = size - 1;
    }
    memcpy(out, path, n);
    out[n] = 0;
}

static esp_err_t sd_locked(const char *path, switch_error_t *e)
{
    uint8_t *raw = NULL;
    uint32_t size = 0;
    st_info_t st;
    esp_err_t err;

    disk_info_t info = { .source = DISK_SRC_SD };
    const char *name = strrchr(path, '/');
    disk_title_from_filename(name ? name + 1 : path, info.name);
    snprintf(info.sd_path, sizeof(info.sd_path), "%s", path);

    if (is_hfe_name(path)) {
        info.hfe = true;                /* read straight into the track buffer */
        return activate_locked(NULL, &info, e);
    }
    if (!is_st_name(path)) {
        return fail(e, "UNSUPPORTED_FORMAT", "only .ST and .HFE images can be inserted%s", "");
    }
    int64_t t0 = esp_timer_get_time();
    sd_result_t r = sd_card_load(path, ST_MAX_SIZE, &raw, &size);
    load_us = esp_timer_get_time() - t0;
    if (r != SD_OK) {
        err = fail(e, r == SD_NO_CARD ? "SD_NOT_PRESENT" : r == SD_NOT_FOUND ? "FILE_NOT_FOUND"
                   : r == SD_INVALID ? "INVALID_IMAGE" : "SD_ERROR", "%s",
                   r == SD_INVALID ? "not an image file (a folder, empty, or larger than an .ST image)"
                   : sd_result_name(r));
    } else if (st_check_image(raw, size, &st) != ST_OK) {
        err = ESP_FAIL;
        e->code = "UNSUPPORTED_GEOMETRY";
        snprintf(e->msg, sizeof(e->msg), "%s", st.detail);
    } else {
        info.size = size;
        info.crc32 = info.file_crc = esp_rom_crc32_le(0, raw, size);
        info.keep_sectors = true;       /* written sectors go back to the file */
        sd_card_get_read_only(path, &info.read_only);
        info.cylinders = st.cylinders;
        info.heads = st.heads;
        info.sectors = st.sectors;
        err = activate_locked(raw, &info, e);
    }
    free(raw);
    return err;
}

void disk_switch_set_enabled(bool on)
{
    enabled = on;
}

void disk_switch_init(void)
{
    lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(remember_task_fn, "remember", 6144, NULL, 1, &remember_task, 1);
    /* Started with a disk from the SD card: the buttons work in its folder,
     * and its copy belongs in the flash. */
    disk_info_t cur;
    disk_get_current(&cur);
    if (cur.source == DISK_SRC_SD) {
        dirname_of(cur.sd_path, browse_dir, sizeof(browse_dir));
    }
    remember(&cur);
}

esp_err_t disk_switch_raw(const uint8_t *raw, const disk_info_t *info, switch_error_t *e)
{
    disk_info_t copy = *info;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = activate_locked(raw, &copy, e);
    xSemaphoreGive(lock);
    return err;
}

esp_err_t disk_switch_psram_upload(uint8_t *raw, const disk_info_t *info, switch_error_t *e)
{
    disk_info_t copy = *info;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = activate_locked(raw, &copy, e);
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

esp_err_t disk_switch_sd(const char *path, switch_error_t *e)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = sd_locked(path, e);
    if (err == ESP_OK) {
        dirname_of(path, browse_dir, sizeof(browse_dir));   /* the buttons follow */
    }
    xSemaphoreGive(lock);
    return err;
}

/* ---- Buttons: the image files of one folder ------------------------------------ */

typedef char step_name_t[SD_NAME_MAX + 1];

/* Names as the web page sorts them: case-insensitive, digit runs by value. */
static int natural_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            size_t la = 0, lb = 0;
            while (isdigit((unsigned char)a[la])) la++;
            while (isdigit((unsigned char)b[lb])) lb++;
            if (la != lb) {
                return la < lb ? -1 : 1;
            }
            int c = strncmp(a, b, la);
            if (c) {
                return c;
            }
            a += la;
            b += lb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        a++;
        b++;
    }
    return *a ? 1 : *b ? -1 : 0;
}

static int name_qsort(const void *x, const void *y)
{
    return natural_cmp(*(const step_name_t *)x, *(const step_name_t *)y);
}

typedef struct {
    step_name_t *names;
    int n;
} step_list_t;

static void step_add(void *ctx, const sd_entry_t *ent)
{
    step_list_t *l = ctx;
    if (!ent->is_dir && l->n < STEP_MAX && strlen(ent->name) <= SD_NAME_MAX &&
        (is_st_name(ent->name) || is_hfe_name(ent->name))) {
        memcpy(l->names[l->n++], ent->name, strlen(ent->name) + 1);
    }
}

esp_err_t disk_switch_step(int dir, switch_error_t *e)
{
    static step_name_t *names;          /* 48 KiB: in PSRAM, not internal RAM */
    char path[SD_REL_MAX + 1];
    bool more;

    if (!enabled) {
        return fail(e, "MACHINE_NOT_SUPPORTED", "floppy emulation is off (computer not "
                    "supported yet)%s", "");
    }
    if (!names) {
        names = heap_caps_malloc(STEP_MAX * sizeof(*names), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!names) {
            return fail(e, "INSUFFICIENT_MEMORY", "no memory for the file list%s", "");
        }
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    step_list_t l = { .names = names };
    sd_result_t r = sd_card_list(browse_dir, STEP_MAX * 4, step_add, &l, &more);
    if (r != SD_OK) {
        xSemaphoreGive(lock);
        return fail(e, r == SD_NO_CARD ? "SD_NOT_PRESENT" : "SD_ERROR", "%s", sd_result_name(r));
    }
    qsort(names, l.n, sizeof(*names), name_qsort);

    /* Where the current disk is in this folder (-1: not here). */
    disk_info_t cur;
    disk_get_current(&cur);
    int pos = -1;
    if (cur.source == DISK_SRC_SD) {
        char cur_dir[SD_REL_MAX + 1];
        dirname_of(cur.sd_path, cur_dir, sizeof(cur_dir));
        const char *cur_name = strrchr(cur.sd_path, '/');
        cur_name = cur_name ? cur_name + 1 : cur.sd_path;
        for (int i = 0; i < l.n && strcmp(cur_dir, browse_dir) == 0; i++) {
            if (strcmp(names[i], cur_name) == 0) {
                pos = i;
            }
        }
    }
    /* Not in this folder: right starts at the first, left at the last. */
    int next = pos < 0 ? (dir > 0 ? 0 : l.n - 1) : pos + dir;
    if (l.n == 0 || next < 0 || next >= l.n) {
        xSemaphoreGive(lock);
        fail(e, "END", "no %s image file in this folder", dir > 0 ? "next" : "previous");
        return ESP_ERR_INVALID_STATE;
    }
    int n = snprintf(path, sizeof(path), "%s%s%s", browse_dir, browse_dir[0] ? "/" : "", names[next]);
    esp_err_t err = n > 0 && (size_t)n < sizeof(path) ? sd_locked(path, e)
                  : fail(e, "INVALID_PATH", "the path of %s is too long", names[next]);
    xSemaphoreGive(lock);
    return err;
}
