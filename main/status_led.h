/*
 * On-board addressable RGB LED (WS2812-type) of the DevKitC: orange while
 * our drive is selected (EMU_SELECT_LINE) and the MOTOR line is active.
 */
#pragma once

#include "esp_err.h"

/* On this Otronic board the RGB LED is on GPIO48 (DevKitC v1.0 layout,
 * confirmed with led_test.c). On the PCB this pin also goes to the
 * unfitted LED_STATUS connector. */
#define STATUS_LED_GPIO     48

/* Orange, dimmed (addressable LEDs are very bright; green is the brightest
 * die, hence a little less green than red). */
#define STATUS_LED_R        8
#define STATUS_LED_G        2
#define STATUS_LED_B        0

/* Start the LED (off) and its update task. */
esp_err_t status_led_init(void);
