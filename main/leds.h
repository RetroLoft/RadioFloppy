/*
 * The front panel LEDs. What they show depends on the "leds" setting (how
 * many LEDs are fitted on J4):
 *
 *   J4 pin 3      disk activity, always: on while our drive is selected
 *                 (EMU_SELECT_LINE) and the MOTOR line is active (armed: an
 *                 unpowered Atari does not count).
 *   WiFi status   2 LEDs: the LED on J4 pin 1. 1 LED: the DevKitC's
 *                 on-board RGB LED (dim blue; GPIO48 is also its data
 *                 line, so it cannot be used next to an LED on pin 1).
 *                 Off while connected; blinking (1 Hz) while joining,
 *                 during start-up or after the connection was lost; on
 *                 when no connection came for LEDS_WIFI_GIVE_UP_MS (it
 *                 keeps trying and goes off once connected) or no network
 *                 is set. Fast blinking (4 Hz) in WiFi setup mode.
 *
 * A change of the setting takes effect at once.
 */
#pragma once

#include "esp_err.h"

#define LEDS_WIFI_GIVE_UP_MS    60000

/* Configure the LEDs (off) and start their update task. */
esp_err_t leds_init(void);
