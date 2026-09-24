/*
 * WiFi station for the HTTP API. Runs on core 1 (the floppy interrupts
 * and the flux stream are on core 0). Credentials come from menuconfig
 * (RadioFloppy menu); nothing is written to the internal flash at run time.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool configured;        /* SSID set in menuconfig */
    bool connected;         /* has an IP address */
    char ssid[33];
    char ip[16];
    int8_t rssi;
} wifi_net_status_t;

/* Start WiFi (returns once the driver is started, not when connected). */
esp_err_t wifi_net_start(void);

void wifi_net_get_status(wifi_net_status_t *st);
