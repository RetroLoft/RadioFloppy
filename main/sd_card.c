/*
 * The SD card in J5. See sd_card.h.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/sdspi_host.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "board_pins.h"
#include "ext_flash.h"
#include "sd_card.h"
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
    settings_t cfg;
    settings_get(&cfg);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!mount_locked()) {
        printf("SD card: none\n");
    } else if (make_root_locked(cfg.root_dir) == ESP_OK) {
        printf("SD card: root directory \"%s\"\n", cfg.root_dir[0] ? cfg.root_dir : "/");
    }
    xSemaphoreGive(lock);
}

void sd_card_status(sd_status_t *st)
{
    settings_t cfg;
    char path[sizeof(SD_MOUNT) + SETTINGS_ROOT_MAX + 1];

    memset(st, 0, sizeof(*st));
    if (!lock) {
        return;
    }
    settings_get(&cfg);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (card && sdmmc_get_status(card) != ESP_OK) {
        unmount_locked();               /* taken out */
    }
    if (!card && esp_timer_get_time() / 1000 - tried_at_ms >= RETRY_MS && mount_locked()) {
        make_root_locked(cfg.root_dir); /* a card inserted since */
    }
    if (card) {
        st->present = true;
        st->size_mb = (uint32_t)((uint64_t)card->csd.capacity * card->csd.sector_size /
                                 (1024 * 1024));
        root_path(cfg.root_dir, path, sizeof(path));
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
