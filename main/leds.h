/*
 * The two LEDs on J4 (plain GPIOs, active high):
 *
 *   LED_ACTIVITY  on while our drive is selected (EMU_SELECT_LINE) and the
 *                 MOTOR line is active (armed: an unpowered Atari does not
 *                 count).
 *   LED_STATUS    WiFi: off while connected; blinking (1 Hz) while joining,
 *                 during start-up or after the connection was lost; on
 *                 when no connection came for LEDS_WIFI_GIVE_UP_MS (it
 *                 keeps trying and goes off once connected) or no network
 *                 is set. Fast blinking (4 Hz) in WiFi setup mode.
 */
#pragma once

#include "esp_err.h"

#define LEDS_WIFI_GIVE_UP_MS    60000

/* Configure both LEDs (off) and start their update task. */
esp_err_t leds_init(void);
