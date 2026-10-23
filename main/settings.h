/*
 * Device settings (host name, WiFi network) on the external flash, two
 * copies at RF_SETTINGS_A / RF_SETTINGS_B (docs/FLASH_LAYOUT.md). Nothing
 * is written to the internal flash, so saving never stalls the floppy
 * emulation. Until settings are saved, the menuconfig values are used.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SETTINGS_HOSTNAME_MAX   32
#define SETTINGS_SSID_MAX       32
#define SETTINGS_PASS_MAX       64
#define SETTINGS_ROOT_MAX       100     /* SD card folder, see settings_root_valid */
#define SETTINGS_SD_PATH_MAX    200     /* = SD_REL_MAX (sd_path.h) */

/* Lowest security accepted when joining the network. */
typedef enum {
    WIFI_SEC_WPA2 = 0,          /* WPA2 or WPA3 Personal (default) */
    WIFI_SEC_WPA3 = 1,          /* WPA3 Personal only */
    WIFI_SEC_WPA = 2,           /* also WPA (older routers) */
    WIFI_SEC_OPEN = 3,          /* no password */
    WIFI_SEC_COUNT
} wifi_security_t;

typedef struct {
    char hostname[SETTINGS_HOSTNAME_MAX + 1];
    char wifi_ssid[SETTINGS_SSID_MAX + 1];
    char wifi_pass[SETTINGS_PASS_MAX + 1];
    uint8_t wifi_security;      /* wifi_security_t */
    uint8_t drive_select;       /* 0 = DS0 (drive A:), 1 = DS1 (drive B:) */
    uint8_t buzzer;             /* 1 = step and button clicks on (default) */
    uint16_t last_image_id;     /* library image active at power-off, 0 = none */
    uint8_t machine;            /* machine_t: connected computer, default ATARI */
    uint8_t compress;           /* 1 = store new read-only images compressed */
    uint8_t leds;               /* LEDs fitted on J4: 1 (pin 3) or 2 (pins 1 and 3), default 1 */
    char root_dir[SETTINGS_ROOT_MAX + 1];   /* folder on the SD card, "" = the card's root */
    char last_sd[SETTINGS_SD_PATH_MAX + 1]; /* disk active at power-off when it came from the
                                               SD card (below root_dir), else "" */
    uint32_t last_sd_crc;       /* CRC-32 of that disk (finds its copy in the flash) */
} settings_t;

/* Read the settings (initialises the external flash if needed). */
esp_err_t settings_init(void);

/* true: the settings come from the external flash, not from menuconfig. */
bool settings_stored(void);

void settings_get(settings_t *out);

/* Validate and store; the new values are returned by settings_get() at once.
 * Any task (serialised internally). */
esp_err_t settings_save(const settings_t *s);

/* Store the last active library image, only if it differs from the stored
 * one (no flash write otherwise). */
esp_err_t settings_set_last_image(uint16_t image_id);

/* The same for a disk from the SD card (a path below root_dir, sd_path.h
 * rules) with its CRC-32: stored, and last_image_id cleared. "" clears it. */
esp_err_t settings_set_last_sd(const char *path, uint32_t crc);

/* RFC 1123 label: 1..32 letters, digits and '-', not starting/ending with '-'. */
bool settings_hostname_valid(const char *h);

/*
 * A root directory for the SD card: "" (the card's root) or one or more
 * folder names separated by '/', at most SETTINGS_ROOT_MAX characters.
 * Names: letters, digits, space and - _ . ( ) & + , ! only; not "." or
 * "..", not starting with '.' or ' ', not ending with '.' or ' '. No '/'
 * at the start or end, no empty names. It is only ever used below the
 * card's mount point, never as a path of its own.
 */
bool settings_root_valid(const char *root);

/* Only the root directory (a whole settings_t is large for small stacks). */
void settings_get_root(char out[SETTINGS_ROOT_MAX + 1]);

/* WPA passphrase: 8..63 printable ASCII or 64 hex digits; "" for open. */
bool settings_password_valid(const char *pass, wifi_security_t sec);

const char *settings_security_name(wifi_security_t sec);    /* "wpa2", ... */
int settings_security_from_name(const char *name);           /* -1: unknown */
