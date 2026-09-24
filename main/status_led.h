/*
 * On-board addressable RGB LED (WS2812-type) of the DevKitC: orange for as
 * long as the floppy MOTOR line is active (LOW).
 */
#pragma once

#include "esp_err.h"

/* On this Otronic board the RGB LED is on GPIO48 (DevKitC v1.0 layout,
 * confirmed with led_test.c). On the PCB this pin also goes to the
 * unfitted LED_STATUS connector. */
#define STATUS_LED_GPIO     48

/* Red-orange, dimmed (addressable LEDs are very bright). */
#define STATUS_LED_R        64
#define STATUS_LED_G        8
#define STATUS_LED_B        0

/* Start the LED (off) and its update task. */
esp_err_t status_led_init(void);
