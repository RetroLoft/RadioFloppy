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
} settings_t;

/* Read the settings (initialises the external flash if needed). */
esp_err_t settings_init(void);

/* true: the settings come from the external flash, not from menuconfig. */
bool settings_stored(void);

void settings_get(settings_t *out);

/* Validate and store; the new values are returned by settings_get() at once. */
esp_err_t settings_save(const settings_t *s);

/* RFC 1123 label: 1..32 letters, digits and '-', not starting/ending with '-'. */
bool settings_hostname_valid(const char *h);

/* WPA passphrase: 8..63 printable ASCII or 64 hex digits; "" for open. */
bool settings_password_valid(const char *pass, wifi_security_t sec);

const char *settings_security_name(wifi_security_t sec);    /* "wpa2", ... */
int settings_security_from_name(const char *name);           /* -1: unknown */
