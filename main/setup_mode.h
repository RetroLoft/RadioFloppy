/*
 * WiFi setup mode. RadioFloppy opens its own access point with a single
 * setup page (network, security, password); the floppy emulation stays
 * off. Started at boot when no network is configured, or on request
 * (both buttons held 10 s): the request survives a software restart in
 * RTC memory, a power cycle or reset clears it.
 *
 * After new WiFi settings are saved RadioFloppy restarts and must join the
 * network within SETUP_VERIFY_MS, else it returns to setup mode with the
 * reason (network not found, wrong password, ...).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SETUP_VERIFY_MS     45000

typedef enum {
    SETUP_NOT_CONFIGURED = 1,   /* no network set */
    SETUP_BUTTONS = 2,          /* both buttons held 10 s */
    SETUP_JOIN_FAILED = 3,      /* new settings did not work */
} setup_reason_t;

/* Once at boot, after settings_init(): read and clear the RTC request. */
void setup_mode_boot(void);

/* true: run setup_mode_run() instead of the floppy emulation. */
bool setup_mode_wanted(void);

/* Normal mode: WiFi settings were just saved, the join must be verified. */
bool setup_mode_verify_pending(void);
void setup_mode_verify_done(void);

/* Restart into setup mode (any task, does not return). */
void setup_mode_restart(setup_reason_t reason);

/* The join with new settings failed: back to setup mode (does not return). */
void setup_mode_join_failed(const char *ssid, uint8_t wifi_reason);

/* Access point, setup page and DNS redirect. Does not return. */
void setup_mode_run(void);
