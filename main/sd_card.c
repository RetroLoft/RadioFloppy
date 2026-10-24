/*
 * The SD card in J5. See sd_card.h.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#include "board_pins.h"
#include "ext_flash.h"
#include "sd_card.h"
#include "sd_path.h"
#include "settings.h"

#define SD_HOST         SPI2_HOST       /* the bus ext_flash sets up */
#define SD_FREQ_KHZ     20000
#define RETRY_MS        10000           /* look for a newly inserted card */

static SemaphoreHandle_t lock;
static sdmmc_card_t *card;              /* NULL: no card mounted */
static int64_t tried_at_ms = -RETRY_MS;

/* With the lock held. */
static bool mount_locked(void)
{
    if (card) {
        return true;
    }
    tried_at_ms = esp_timer_get_time() / 1000;
    if (ext_flash_init() != ESP_OK) {   /* sets up the shared SPI bus (once) */
        return false;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = SD_HOST;
    dev.gpio_cs = PIN_SD_CS;
    const esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,    /* never format a card */
        .max_files = 4,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &dev, &mc, &card);
    if (err != ESP_OK) {
        card = NULL;
        return false;
    }
    printf("SD card: %s, %lu MB, mounted at %s\n", card->cid.name,
           (unsigned long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024)),
           SD_MOUNT);
    return true;
}

static void unmount_locked(void)
{
    if (card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, card);
        card = NULL;
        printf("SD card: removed\n");
    }
}

/* SD_MOUNT "/" root, or SD_MOUNT for the card's root. root must be valid. */
static void root_path(const char *root, char *out, size_t size)
{
    snprintf(out, size, root[0] ? SD_MOUNT "/%s" : SD_MOUNT, root);
}

/* A folder that can be opened (stat() does not work on SD_MOUNT itself). */
static bool is_dir(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        closedir(d);
    }
    return d != NULL;
}

/* Every folder of the path, one after the other. With the lock held. */
static esp_err_t make_root_locked(const char *root)
{
    char path[sizeof(SD_MOUNT) + SETTINGS_ROOT_MAX + 1];

    root_path(root, path, sizeof(path));
    for (char *p = path + strlen(SD_MOUNT) + 1; root[0]; p++) {
        if (*p == '/' || *p == 0) {
            char c = *p;
            *p = 0;
            if (!is_dir(path)) {
                if (mkdir(path, 0777) != 0 && errno != EEXIST) {
                    printf("SD card: cannot create %s (errno %d)\n", path, errno);
                    return ESP_FAIL;
                }
                printf("SD card: created %s\n", path);
            }
            *p = c;
            if (!c) {
                break;
            }
        }
    }
    return is_dir(path) ? ESP_OK : ESP_FAIL;
}

void sd_card_init(void)
{
    if (!lock) {
        lock = xSemaphoreCreateMutex();
    }
    char root[SETTINGS_ROOT_MAX + 1];
    settings_get_root(root);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!mount_locked()) {
        printf("SD card: none\n");
    } else if (make_root_locked(root) == ESP_OK) {
        printf("SD card: root directory \"%s\"\n", root[0] ? root : "/");
    }
    xSemaphoreGive(lock);
}

bool sd_card_present(void)
{
    return card != NULL;
}

void sd_card_status(sd_status_t *st)
{
    char root[SETTINGS_ROOT_MAX + 1];
    char path[sizeof(SD_MOUNT) + SETTINGS_ROOT_MAX + 1];

    memset(st, 0, sizeof(*st));
    if (!lock) {
        return;
    }
    settings_get_root(root);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (card && sdmmc_get_status(card) != ESP_OK) {
        unmount_locked();               /* taken out */
    }
    if (!card && esp_timer_get_time() / 1000 - tried_at_ms >= RETRY_MS && mount_locked()) {
        make_root_locked(root);         /* a card inserted since */
    }
    if (card) {
        st->present = true;
        st->size_mb = (uint32_t)((uint64_t)card->csd.capacity * card->csd.sector_size /
                                 (1024 * 1024));
        root_path(root, path, sizeof(path));
        st->root_exists = is_dir(path);
    }
    xSemaphoreGive(lock);
}

esp_err_t sd_card_make_root(const char *root)
{
    if (!settings_root_valid(root)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (card && sdmmc_get_status(card) != ESP_OK) {
        unmount_locked();
    }
    esp_err_t err = !card && !mount_locked() ? ESP_ERR_NOT_FOUND : make_root_locked(root);
    xSemaphoreGive(lock);
    return err;
}

/* ---- Folders and files ------------------------------------------------------------ */

#define PATH_MAX_LEN    (sizeof(SD_MOUNT) + SETTINGS_ROOT_MAX + SD_REL_MAX + 3)
#define TMP_NAME        ".rf-upload.tmp"
#define OLD_NAME        ".rf-replaced.tmp"
#define RM_DEPTH        12              /* recursive delete: folder levels */

const char *sd_result_name(sd_result_t r)
{
    switch (r) {
    case SD_OK:         return "ok";
    case SD_NO_CARD:    return "no SD card";
    case SD_INVALID:    return "invalid path";
    case SD_NOT_FOUND:  return "not found";
    case SD_EXISTS:     return "already exists";
    case SD_NOT_EMPTY:  return "folder not empty";
    case SD_READ_ONLY:  return "the file is read-only";
    default:            return "SD card error";
    }
}

/* Lock taken, card checked, full path of rel built. */
static sd_result_t begin(const char *rel, char *path)
{
    char root[SETTINGS_ROOT_MAX + 1];

    if (!lock) {
        return SD_NO_CARD;
    }
    if (!sd_relpath_valid(rel)) {
        return SD_INVALID;
    }
    settings_get_root(root);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (card && sdmmc_get_status(card) != ESP_OK) {
        unmount_locked();
    }
    if (!card && !mount_locked()) {
        xSemaphoreGive(lock);
        return SD_NO_CARD;
    }
    if (!sd_path_join(path, PATH_MAX_LEN, SD_MOUNT, root, rel)) {
        xSemaphoreGive(lock);
        return SD_INVALID;
    }
    return SD_OK;
}

static bool is_file(const char *path, uint32_t *size)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (size) {
        *size = (uint32_t)st.st_size;
    }
    return true;
}

/* The FAT read-only attribute of path (lock held). The VFS has no chmod:
 * FatFs itself, on "<drive>:/<path on the card>". */
static bool set_ro_locked(const char *path, bool ro)
{
    char fpath[PATH_MAX_LEN + 260];
    snprintf(fpath, sizeof(fpath), "%u:%s", ff_diskio_get_pdrv_card(card), path + strlen(SD_MOUNT));
    return f_chmod(fpath, ro ? AM_RDO : 0, AM_RDO) == FR_OK;
}

static bool is_read_only(const char *path)
{
    return access(path, W_OK) != 0;     /* the FAT VFS: AM_RDO refuses W_OK */
}

/* Delete a file, also a read-only one (deleting is asked for explicitly). */
static int unlink_any(const char *path)
{
    if (unlink(path) == 0) {
        return 0;
    }
    return is_read_only(path) && set_ro_locked(path, false) ? unlink(path) : -1;
}

sd_result_t sd_card_list(const char *rel, int max, void (*cb)(void *ctx, const sd_entry_t *e),
                         void *ctx, bool *more)
{
    char path[PATH_MAX_LEN], child[PATH_MAX_LEN + 256 + 2];   /* FAT names up to 255 */
    sd_result_t r = begin(rel, path);

    *more = false;
    if (r != SD_OK) {
        return r;
    }
    DIR *d = opendir(path);
    if (!d) {
        xSemaphoreGive(lock);
        return SD_NOT_FOUND;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;                   /* hidden, also our temporary files */
        }
        if (n == max) {
            *more = true;
            break;
        }
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        sd_entry_t ent = { .name = e->d_name, .is_dir = e->d_type == DT_DIR };
        if (!ent.is_dir) {
            is_file(child, &ent.size);
            ent.read_only = is_read_only(child);
        }
        cb(ctx, &ent);
        n++;
    }
    closedir(d);
    xSemaphoreGive(lock);
    return SD_OK;
}

sd_result_t sd_card_stat(const char *rel, bool *dir, uint32_t *size)
{
    char path[PATH_MAX_LEN];
    sd_result_t r = begin(rel, path);

    if (r != SD_OK) {
        return r;
    }
    *size = 0;
    *dir = is_dir(path);
    if (!*dir && !is_file(path, size)) {
        r = SD_NOT_FOUND;
    }
    xSemaphoreGive(lock);
    return r;
}

sd_result_t sd_card_get_read_only(const char *rel, bool *ro)
{
    char path[PATH_MAX_LEN];
    sd_result_t r = begin(rel, path);

    if (r != SD_OK) {
        return r;
    }
    if (!rel[0] || !is_file(path, NULL)) {
        r = SD_NOT_FOUND;
    } else {
        *ro = is_read_only(path);
    }
    xSemaphoreGive(lock);
    return r;
}

sd_result_t sd_card_set_read_only(const char *rel, bool ro)
{
    char path[PATH_MAX_LEN];
    sd_result_t r = begin(rel, path);

    if (r != SD_OK) {
        return r;
    }
    if (!rel[0] || !is_file(path, NULL)) {
        r = SD_NOT_FOUND;
    } else {
        if (!set_ro_locked(path, ro)) {
            r = SD_IO;
        } else {
            printf("SD card: %s %s\n", path, ro ? "read-only" : "writable");
        }
    }
    xSemaphoreGive(lock);
    return r;
}

sd_result_t sd_card_update(const char *rel, const uint8_t *data, uint32_t size, uint32_t mask,
                           uint32_t region)
{
    char path[PATH_MAX_LEN];
    sd_result_t r = begin(rel, path);
    uint32_t n = 0;

    if (r != SD_OK) {
        return r;
    }
    FILE *f = NULL;
    if (!rel[0] || !is_file(path, &n)) {
        r = SD_NOT_FOUND;
    } else if (n != size) {
        r = SD_INVALID;
    } else if (is_read_only(path)) {
        r = SD_READ_ONLY;
    } else if (!(f = fopen(path, "r+b"))) {
        r = SD_IO;
    } else {
        setvbuf(f, NULL, _IONBF, 0);
        for (uint32_t i = 0; i < 32 && r == SD_OK; i++) {
            uint32_t off = i * region;
            if (!(mask & (1u << i)) || off >= size) {
                continue;
            }
            uint32_t len = size - off < region ? size - off : region;
            if (fseek(f, off, SEEK_SET) != 0 || fwrite(data + off, 1, len, f) != len) {
                r = SD_IO;
            }
        }
        if (fsync(fileno(f)) != 0) {
            r = SD_IO;
        }
        if (fclose(f) != 0) {
            r = SD_IO;
        }
    }
    xSemaphoreGive(lock);
    return r;
}

sd_result_t sd_card_save_changes(const char *rel, const uint8_t *data, uint32_t size,
                                 uint32_t crc, char *out, size_t out_size)
{
    char folder[SD_REL_MAX + 1], name[SD_NAME_MAX + 1];
    const char *slash = strrchr(rel, '/');
    const char *base = slash ? slash + 1 : rel;
    size_t flen = slash ? (size_t)(slash - rel) : 0;
    size_t stem = strlen(base);

    if (flen >= sizeof(folder)) {
        return SD_INVALID;
    }
    memcpy(folder, rel, flen);
    folder[flen] = 0;
    if (stem >= 3 && strcasecmp(base + stem - 3, ".st") == 0) {
        stem -= 3;
    }
    sd_result_t r = SD_EXISTS;
    for (int i = 1; i <= 9 && r == SD_EXISTS; i++) {
        char tag[16];
        snprintf(tag, sizeof(tag), i == 1 ? " (changes).st" : " (changes %d).st", i);
        int keep = (int)(stem < SD_NAME_MAX - strlen(tag) ? stem : SD_NAME_MAX - strlen(tag));
        while (keep > 0 && (base[keep - 1] == ' ' || base[keep - 1] == '.')) {
            keep--;                     /* no name ending in a space or dot before the tag */
        }
        snprintf(name, sizeof(name), "%.*s%s", keep, base, tag);
        sd_writer_t *w;
        r = sd_card_writer_open(folder, name, false, &w);
        if (r == SD_OK) {
            r = sd_card_writer_write(w, data, size);
            if (r == SD_OK) {
                r = sd_card_writer_commit(w, crc);
            } else {
                sd_card_writer_abort(w);
            }
        }
    }
    if (r == SD_OK) {
        snprintf(out, out_size, "%s%s%s", folder, folder[0] ? "/" : "", name);
    }
    return r;
}

sd_result_t sd_card_file_crc(const char *rel, uint32_t *crc, uint32_t *size)
{
    const uint32_t piece = 8192;
    sd_reader_t *rd = NULL;
    uint32_t got, done = 0, c = 0;

    *crc = *size = 0;
    uint8_t *buf = heap_caps_malloc(piece, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return SD_IO;
    }
    sd_result_t r = sd_card_reader_open(rel, UINT32_MAX, &rd, size);
    while (r == SD_OK && done < *size) {
        uint32_t want = *size - done < piece ? *size - done : piece;
        r = sd_card_reader_read(rd, buf, want, &got);
        c = esp_rom_crc32_le(c, buf, got);
        done += got;
    }
    sd_card_reader_close(rd);
    free(buf);
    *crc = c;
    return r;
}

sd_result_t sd_card_mkdir(const char *rel)
{
    char path[PATH_MAX_LEN];
    sd_result_t r = begin(rel, path);

    if (r != SD_OK) {
        return r;
    }
    if (!rel[0] || is_dir(path)) {
        r = SD_EXISTS;
    } else if (is_file(path, NULL)) {
        r = SD_EXISTS;                  /* a file of that name */
    } else {
        /* Every folder on the way, from the root directory down. */
        size_t start = strlen(path) - strlen(rel);
        for (size_t i = start; r == SD_OK; i++) {
            if (path[i] == '/' || path[i] == 0) {
                char c = path[i];
                path[i] = 0;
                if (!is_dir(path) && mkdir(path, 0777) != 0) {
                    printf("SD card: cannot create %s (errno %d)\n", path, errno);
                    r = SD_IO;
                }
                path[i] = c;
                if (!c) {
                    break;
                }
            }
        }
        if (r == SD_OK) {
            printf("SD card: created folder %s\n", path);
        }
    }
    xSemaphoreGive(lock);
    return r;
}

/* Everything in path, then path itself. With the lock held. */
static sd_result_t remove_tree(char *path, size_t size, int depth)
{
    if (depth > RM_DEPTH) {
        return SD_IO;
    }
    DIR *d = opendir(path);
    if (!d) {
        return SD_IO;
    }
    size_t len = strlen(path);
    sd_result_t r = SD_OK;
    struct dirent *e;
    while (r == SD_OK && (e = readdir(d)) != NULL) {
        if (len + 1 + strlen(e->d_name) + 1 > size) {
            r = SD_IO;
            break;
        }
        snprintf(path + len, size - len, "/%s", e->d_name);
        if (e->d_type == DT_DIR) {
            r = remove_tree(path, size, depth + 1);
        } else if (unlink_any(path) != 0) {
            r = SD_IO;
        }
        path[len] = 0;
    }
    closedir(d);
    if (r == SD_OK && rmdir(path) != 0) {
        r = SD_IO;
    }
    return r;
}

sd_result_t sd_card_rmdir(const char *rel, bool recursive)
{
    char path[PATH_MAX_LEN + 256];
    sd_result_t r = begin(rel, path);

    if (r != SD_OK) {
        return r;
    }
    if (!rel[0]) {
        r = SD_INVALID;                 /* never the root directory itself */
    } else if (!is_dir(path)) {
        r = SD_NOT_FOUND;
    } else if (recursive) {
        r = remove_tree(path, sizeof(path), 0);
    } else if (rmdir(path) != 0) {
        r = errno == ENOTEMPTY || errno == EEXIST || errno == EACCES ? SD_NOT_EMPTY : SD_IO;
        /* FatFs reports a non-empty folder as EACCES or ENOTEMPTY */
        if (r == SD_NOT_EMPTY) {
            DIR *d = opendir(path);
            struct dirent *e = d ? readdir(d) : NULL;
            if (d) {
                closedir(d);
            }
            if (!e) {
                r = SD_IO;              /* empty after all: a real error */
            }
        }
    }
    if (r == SD_OK) {
        printf("SD card: removed folder %s%s\n", path, recursive ? " with its contents" : "");
    }
    xSemaphoreGive(lock);
    return r;
}

sd_result_t sd_card_remove(const char *rel)
{
    char path[PATH_MAX_LEN];
    sd_result_t r = begin(rel, path);

    if (r != SD_OK) {
        return r;
    }
    if (!rel[0] || !is_file(path, NULL)) {
        r = SD_NOT_FOUND;
    } else if (unlink_any(path) != 0) {
        r = SD_IO;
    } else {
        printf("SD card: removed %s\n", path);
    }
    xSemaphoreGive(lock);
    return r;
}

struct sd_writer {
    FILE *f;
    bool overwrite;
    char target[PATH_MAX_LEN + SD_NAME_MAX + 2];
    char tmp[PATH_MAX_LEN + sizeof(TMP_NAME) + 2];
    char old[PATH_MAX_LEN + sizeof(OLD_NAME) + 2];
};

sd_result_t sd_card_writer_open(const char *folder, const char *name, bool overwrite,
                                sd_writer_t **wp)
{
    char path[PATH_MAX_LEN];
    sd_result_t r;

    *wp = NULL;
    if (!sd_name_valid(name)) {
        return SD_INVALID;
    }
    if ((r = begin(folder, path)) != SD_OK) {
        return r;
    }
    sd_writer_t *w = calloc(1, sizeof(*w));
    if (!w) {
        r = SD_IO;
    } else if (!is_dir(path)) {
        r = SD_NOT_FOUND;
    } else {
        snprintf(w->target, sizeof(w->target), "%s/%s", path, name);
        snprintf(w->tmp, sizeof(w->tmp), "%s/" TMP_NAME, path);
        snprintf(w->old, sizeof(w->old), "%s/" OLD_NAME, path);
        w->overwrite = overwrite;
        if (is_dir(w->target) || (!overwrite && is_file(w->target, NULL))) {
            r = SD_EXISTS;
        } else if (is_file(w->target, NULL) && is_read_only(w->target)) {
            r = SD_READ_ONLY;
        } else {
            unlink_any(w->tmp);             /* left over from an interrupted upload */
            w->f = fopen(w->tmp, "wb");
            r = w->f ? SD_OK : SD_IO;
            if (w->f) {
                setvbuf(w->f, NULL, _IONBF, 0);     /* the caller writes whole pieces */
            }
        }
    }
    xSemaphoreGive(lock);
    if (r != SD_OK) {
        free(w);
        return r;
    }
    *wp = w;
    return SD_OK;
}

sd_result_t sd_card_writer_write(sd_writer_t *w, const void *data, uint32_t len)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    size_t n = card ? fwrite(data, 1, len, w->f) : 0;
    xSemaphoreGive(lock);
    return n == len ? SD_OK : SD_IO;
}

void sd_card_writer_abort(sd_writer_t *w)
{
    if (!w) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (w->f) {
        fclose(w->f);
    }
    if (card) {
        unlink(w->tmp);
    }
    xSemaphoreGive(lock);
    free(w);
}

sd_result_t sd_card_writer_commit(sd_writer_t *w, uint32_t crc)
{
    static uint8_t buf[4096];
    sd_result_t r = SD_OK;

    xSemaphoreTake(lock, portMAX_DELAY);
    bool closed = w->f && fclose(w->f) == 0;
    w->f = NULL;
    if (!card || !closed) {
        r = SD_IO;
    }
    /* Read it back: what is on the card must be what was received. */
    FILE *f = r == SD_OK ? fopen(w->tmp, "rb") : NULL;
    if (r == SD_OK) {
        uint32_t c = 0;
        size_t n;
        if (!f) {
            r = SD_IO;
        } else {
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                c = esp_rom_crc32_le(c, buf, n);
            }
            fclose(f);
            if (c != crc) {
                printf("SD card: %s reads back with CRC %08lx, expected %08lx\n", w->tmp,
                       (unsigned long)c, (unsigned long)crc);
                r = SD_IO;
            }
        }
    }
    if (r == SD_OK) {
        bool had = is_file(w->target, NULL);
        if (had && !w->overwrite) {
            r = SD_EXISTS;              /* appeared meanwhile */
        } else if (had && is_read_only(w->target)) {
            r = SD_READ_ONLY;           /* made read-only meanwhile */
        } else if (had) {
            /* Old one aside, new one in place, old one away. */
            unlink_any(w->old);
            if (rename(w->target, w->old) != 0 || rename(w->tmp, w->target) != 0) {
                rename(w->old, w->target);      /* put the old one back */
                r = SD_IO;
            } else {
                unlink(w->old);
            }
        } else if (rename(w->tmp, w->target) != 0) {
            r = SD_IO;
        }
    }
    if (r != SD_OK && card) {
        unlink(w->tmp);
    }
    if (r == SD_OK) {
        printf("SD card: stored %s\n", w->target);
    }
    xSemaphoreGive(lock);
    free(w);
    return r;
}

sd_result_t sd_card_load(const char *rel, uint32_t max, uint8_t **buf, uint32_t *size)
{
    char path[PATH_MAX_LEN];
    sd_result_t r = begin(rel, path);

    *buf = NULL;
    *size = 0;
    if (r != SD_OK) {
        return r;
    }
    uint32_t n = 0;
    if (!rel[0] || !is_file(path, &n)) {
        r = is_dir(path) ? SD_INVALID : SD_NOT_FOUND;
    } else if (n == 0 || n > max) {
        r = SD_INVALID;
    } else {
        uint8_t *b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        FILE *f = b ? fopen(path, "rb") : NULL;
        if (!f) {
            free(b);
            r = SD_IO;
        } else {
            size_t got = fread(b, 1, n, f);
            fclose(f);
            if (got != n) {
                free(b);
                r = SD_IO;
            } else {
                *buf = b;
                *size = n;
            }
        }
    }
    xSemaphoreGive(lock);
    return r;
}

struct sd_reader {
    FILE *f;
};

sd_result_t sd_card_reader_open(const char *rel, uint32_t max, sd_reader_t **rp, uint32_t *size)
{
    char path[PATH_MAX_LEN];
    sd_result_t r = begin(rel, path);

    *rp = NULL;
    *size = 0;
    if (r != SD_OK) {
        return r;
    }
    uint32_t n = 0;
    sd_reader_t *rd = NULL;
    if (!rel[0] || !is_file(path, &n)) {
        r = is_dir(path) ? SD_INVALID : SD_NOT_FOUND;
    } else if (n == 0 || n > max) {
        r = SD_INVALID;
    } else if (!(rd = calloc(1, sizeof(*rd))) || !(rd->f = fopen(path, "rb"))) {
        free(rd);
        rd = NULL;
        r = SD_IO;
    } else {
        *size = n;
    }
    xSemaphoreGive(lock);
    *rp = rd;
    return r;
}

sd_result_t sd_card_reader_read(sd_reader_t *rd, void *buf, uint32_t len, uint32_t *got)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    size_t n = card ? fread(buf, 1, len, rd->f) : 0;
    xSemaphoreGive(lock);
    *got = (uint32_t)n;
    return n > 0 ? SD_OK : SD_IO;
}

void sd_card_reader_close(sd_reader_t *rd)
{
    if (!rd) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    fclose(rd->f);
    xSemaphoreGive(lock);
    free(rd);
}
