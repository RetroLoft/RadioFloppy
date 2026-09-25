/*
 * Device settings. See settings.h and docs/FLASH_LAYOUT.md.
 *
 * Same scheme as the slot catalog: a save goes to the other copy
 * (generation + 1), is read back and only then gets its commit word; the
 * valid copy with the highest generation wins. A power cut during a save
 * leaves the previous settings in place.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_rom_crc.h"

#include "ext_flash.h"
#include "flash_layout.h"
#include "settings.h"

#ifdef SETTINGS_NO_LOCK                 /* host tests: single threaded */
#define settings_lock()
#define settings_unlock()
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t mutex;
#define settings_lock()     xSemaphoreTake(mutex, portMAX_DELAY)
#define settings_unlock()   xSemaphoreGive(mutex)
#endif

#ifndef SETTINGS_DEFAULT_HOSTNAME       /* host tests define their own */
#include "sdkconfig.h"
#define SETTINGS_DEFAULT_HOSTNAME   CONFIG_RADIOFLOPPY_HOSTNAME
#define SETTINGS_DEFAULT_SSID       CONFIG_RADIOFLOPPY_WIFI_SSID
#define SETTINGS_DEFAULT_PASS       CONFIG_RADIOFLOPPY_WIFI_PASSWORD
#endif

#define SET_MAGIC       0x54534652  /* "RFST" little endian */
#define SET_VERSION     3           /* 2: + drive_select, 3: + buzzer_off, last_image_id */
#define SET_V1_SIZE     148         /* version 1 records are still read */
#define SET_COMMIT      0x21544d43  /* "CMT!", last word of the sector */
#define COMMIT_OFFSET   (RF_SECTOR_SIZE - 4)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;              /* sizeof(record_t) */
    uint32_t generation;
    uint32_t crc32;             /* whole record, this field = 0 */
    char hostname[SETTINGS_HOSTNAME_MAX + 1];
    char wifi_ssid[SETTINGS_SSID_MAX + 1];
    char wifi_pass[SETTINGS_PASS_MAX + 1];
    uint8_t wifi_security;
    uint8_t drive_select;       /* since version 2 */
    uint8_t buzzer_off;         /* since version 3 (was 0 padding in 2): 0 = on */
    uint16_t last_image_id;     /* since version 3 (was 0 padding in 2): 0 = none */
} record_t;

_Static_assert(sizeof(record_t) == 152, "settings record layout");

static settings_t current;
static uint32_t generation;
static int current_copy = -1;   /* 0 = A, 1 = B, -1 = nothing stored */

static const uint32_t copy_addr[2] = { RF_SETTINGS_A, RF_SETTINGS_B };

/* CRC over the first len bytes of the record (sizeof for v2, 148 for v1). */
static uint32_t record_crc(const record_t *r, size_t len)
{
    record_t tmp = *r;
    tmp.crc32 = 0;
    return esp_rom_crc32_le(0, (const uint8_t *)&tmp, len);
}

static bool terminated(const char *s, size_t size)
{
    return memchr(s, 0, size) != NULL;
}

static bool read_copy(int copy, record_t *r)
{
    uint32_t commit = 0;

    if (ext_flash_read(copy_addr[copy], r, sizeof(*r)) != ESP_OK ||
        ext_flash_read(copy_addr[copy] + COMMIT_OFFSET, &commit, sizeof(commit)) != ESP_OK) {
        return false;
    }
    bool v1 = r->version == 1 && r->size == SET_V1_SIZE;
    if (v1) {
        r->drive_select = 1;            /* before version 2 always DS1 (B:) */
        r->buzzer_off = 0;
        r->last_image_id = 0;
    }
    bool layout = v1 ? r->crc32 == record_crc(r, SET_V1_SIZE)
                     : (r->version == 2 || r->version == SET_VERSION) && r->size == sizeof(*r) &&
                       r->crc32 == record_crc(r, sizeof(*r)) && r->drive_select <= 1;
    return r->magic == SET_MAGIC && commit == SET_COMMIT && layout &&
           terminated(r->hostname, sizeof(r->hostname)) &&
           terminated(r->wifi_ssid, sizeof(r->wifi_ssid)) &&
           terminated(r->wifi_pass, sizeof(r->wifi_pass)) &&
           r->wifi_security < WIFI_SEC_COUNT && r->buzzer_off <= 1;
}

static void defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->hostname, sizeof(s->hostname), "%s",
             settings_hostname_valid(SETTINGS_DEFAULT_HOSTNAME) ? SETTINGS_DEFAULT_HOSTNAME
                                                                : "RadioFloppy");
    snprintf(s->wifi_ssid, sizeof(s->wifi_ssid), "%s", SETTINGS_DEFAULT_SSID);
    snprintf(s->wifi_pass, sizeof(s->wifi_pass), "%s", SETTINGS_DEFAULT_PASS);
    s->wifi_security = s->wifi_pass[0] ? WIFI_SEC_WPA2 : WIFI_SEC_OPEN;
    s->drive_select = 1;                /* DS1: drive B: */
    s->buzzer = 1;
    s->last_image_id = 0;
}

esp_err_t settings_init(void)
{
    record_t r[2];
    bool ok[2];

#ifndef SETTINGS_NO_LOCK
    if (!mutex) {
        mutex = xSemaphoreCreateMutex();
    }
#endif
    defaults(&current);
    current_copy = -1;
    generation = 0;
    if (ext_flash_init() != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < 2; i++) {
        ok[i] = read_copy(i, &r[i]);
    }
    int best = ok[0] && (!ok[1] || r[0].generation >= r[1].generation) ? 0 : ok[1] ? 1 : -1;
    if (best < 0) {
        return ESP_OK;                  /* nothing saved yet: menuconfig values */
    }
    memcpy(current.hostname, r[best].hostname, sizeof(current.hostname));
    memcpy(current.wifi_ssid, r[best].wifi_ssid, sizeof(current.wifi_ssid));
    memcpy(current.wifi_pass, r[best].wifi_pass, sizeof(current.wifi_pass));
    current.wifi_security = r[best].wifi_security;
    current.drive_select = r[best].drive_select;
    current.buzzer = !r[best].buzzer_off;
    current.last_image_id = r[best].last_image_id;
    generation = r[best].generation;
    current_copy = best;
    return ESP_OK;
}

bool settings_stored(void)
{
    return current_copy >= 0;
}

void settings_get(settings_t *out)
{
    settings_lock();
    *out = current;
    settings_unlock();
}

static esp_err_t save_locked(const settings_t *s);

esp_err_t settings_save(const settings_t *s)
{
    settings_lock();
    esp_err_t err = save_locked(s);
    settings_unlock();
    return err;
}

esp_err_t settings_set_last_image(uint16_t image_id)
{
    esp_err_t err = ESP_OK;

    settings_lock();
    if (current.last_image_id != image_id) {
        settings_t s = current;
        s.last_image_id = image_id;
        err = save_locked(&s);
    }
    settings_unlock();
    return err;
}

static esp_err_t save_locked(const settings_t *s)
{
    if (!ext_flash_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!terminated(s->hostname, sizeof(s->hostname)) ||
        !terminated(s->wifi_ssid, sizeof(s->wifi_ssid)) ||
        !terminated(s->wifi_pass, sizeof(s->wifi_pass)) ||
        !settings_hostname_valid(s->hostname) || s->wifi_security >= WIFI_SEC_COUNT ||
        s->drive_select > 1 || s->buzzer > 1 ||
        (s->wifi_ssid[0] && !settings_password_valid(s->wifi_pass, s->wifi_security))) {
        return ESP_ERR_INVALID_ARG;
    }

    record_t r, check;
    memset(&r, 0, sizeof(r));
    r.magic = SET_MAGIC;
    r.version = SET_VERSION;
    r.size = sizeof(r);
    r.generation = generation + 1;
    memcpy(r.hostname, s->hostname, sizeof(r.hostname));
    memcpy(r.wifi_ssid, s->wifi_ssid, sizeof(r.wifi_ssid));
    memcpy(r.wifi_pass, s->wifi_pass, sizeof(r.wifi_pass));
    r.wifi_security = s->wifi_security;
    r.drive_select = s->drive_select;
    r.buzzer_off = !s->buzzer;
    r.last_image_id = s->last_image_id;
    r.crc32 = record_crc(&r, sizeof(r));

    /* Never touch the copy in use. */
    int target = current_copy == 0 ? 1 : 0;
    uint32_t addr = copy_addr[target];
    const uint32_t commit = SET_COMMIT;
    esp_err_t err;
    if ((err = ext_flash_erase(addr, RF_SECTOR_SIZE)) != ESP_OK ||
        (err = ext_flash_write(addr, &r, sizeof(r))) != ESP_OK ||
        (err = ext_flash_read(addr, &check, sizeof(check))) != ESP_OK) {
        return err;
    }
    if (memcmp(&r, &check, sizeof(r)) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    if ((err = ext_flash_write(addr + COMMIT_OFFSET, &commit, sizeof(commit))) != ESP_OK) {
        return err;
    }
    current = *s;
    generation = r.generation;
    current_copy = target;
    return ESP_OK;
}

bool settings_hostname_valid(const char *h)
{
    size_t n = strlen(h);

    if (n == 0 || n > SETTINGS_HOSTNAME_MAX || h[0] == '-' || h[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

bool settings_password_valid(const char *pass, wifi_security_t sec)
{
    size_t n = strlen(pass);

    if (sec == WIFI_SEC_OPEN) {
        return n == 0;
    }
    if (n == 64) {                      /* raw PSK in hex */
        for (size_t i = 0; i < n; i++) {
            char c = pass[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        return true;
    }
    if (n < 8 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (pass[i] < 0x20 || pass[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

static const char *const security_names[WIFI_SEC_COUNT] = {
    [WIFI_SEC_WPA2] = "wpa2",
    [WIFI_SEC_WPA3] = "wpa3",
    [WIFI_SEC_WPA] = "wpa",
    [WIFI_SEC_OPEN] = "open",
};

const char *settings_security_name(wifi_security_t sec)
{
    return sec < WIFI_SEC_COUNT ? security_names[sec] : "wpa2";
}

int settings_security_from_name(const char *name)
{
    for (int i = 0; i < WIFI_SEC_COUNT; i++) {
        if (strcmp(name, security_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}
