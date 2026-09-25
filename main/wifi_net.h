/*
 * WiFi for the HTTP API. Runs on core 1 (the floppy interrupts and the
 * flux stream are on core 0). The network comes from the settings
 * (settings.h: external flash, else menuconfig); nothing is written to the
 * internal flash at run time.
 *
 * Normal mode: station on the configured network.
 * Setup mode: access point for the setup page, plus a station interface
 * that is only used to scan for networks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

typedef struct {
    bool configured;        /* a network is set */
    bool connected;         /* has an IP address (station) */
    bool ap_mode;           /* setup mode: own access point */
    char ssid[33];          /* network joined, or own AP in setup mode */
    char ip[16];
    char hostname[33];      /* in use since start-up */
    int8_t rssi;
} wifi_net_status_t;

/*
 * Join the configured network (returns once the driver is started, not
 * when connected). With verify_ms > 0 the join must succeed within that
 * time, else setup_mode_join_failed() is called (new settings check).
 */
esp_err_t wifi_net_start(uint32_t verify_ms);

/* Setup mode: open an access point (WPA2 with `pass`). IP in ip_out. */
esp_err_t wifi_net_start_ap(const char *ssid, const char *pass, char ip_out[16]);

/* Setup mode: scan for networks (blocks a few seconds). Returns the count. */
int wifi_net_scan(wifi_ap_record_t *out, int max);

void wifi_net_get_status(wifi_net_status_t *st);
