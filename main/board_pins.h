/*
 * Pin mapping of the FloppyEmulator-ESP32S3 PCB (prototype v1).
 *
 * Source: "Floppy emulator.kicad_pcb" (U1 footprint pads), verified against
 * the ESP32-S3-DevKitC-1 header pinout on 2026-09-23. All pins match.
 * Only the BTN_* pins are used in the current (button test) phase;
 * everything else is documented here for later phases and must NOT
 * be initialised yet.
 */
#pragma once

#include "driver/gpio.h"

/* Push buttons: switch to GND, use internal pull-up, active low.
 * Physically (verified on the board) SW_PREV is the RIGHT button and
 * SW_NEXT the LEFT one - the opposite of the original design notes. */
#define PIN_BTN_PREV        GPIO_NUM_8   /* SW1 / SW_PREV - right */
#define PIN_BTN_NEXT        GPIO_NUM_19  /* SW2 / SW_NEXT - left (also native USB D-) */

/* Console: UART0 via the DevKitC USB-to-UART bridge. */
#define PIN_UART0_TX        GPIO_NUM_43
#define PIN_UART0_RX        GPIO_NUM_44

/* ---- Later phases: not used yet --------------------------------------- */

/* Shugart inputs (Atari -> ESP32) through SN74LVC245A, active low. */
#define PIN_FDD_DS0         GPIO_NUM_4
#define PIN_FDD_DS1         GPIO_NUM_5
#define PIN_FDD_MOTOR       GPIO_NUM_6
#define PIN_FDD_DIR         GPIO_NUM_7
#define PIN_FDD_STEP        GPIO_NUM_15
#define PIN_FDD_WDATA       GPIO_NUM_16
#define PIN_FDD_WGATE       GPIO_NUM_17
#define PIN_FDD_SIDE        GPIO_NUM_18

/* Shugart outputs (ESP32 -> Atari) through ULN2003A (open collector):
 * GPIO HIGH = line pulled LOW on the connector = signal asserted. */
#define PIN_FDD_INDEX       GPIO_NUM_1
#define PIN_FDD_TRK0        GPIO_NUM_2
#define PIN_FDD_WPROT       GPIO_NUM_42
#define PIN_FDD_RDATA       GPIO_NUM_41
#define PIN_FDD_READY       GPIO_NUM_40
#define PIN_FDD_DSKCHG      GPIO_NUM_39

/* External SPI NOR flash (W25Q128JVS in schematic, verify fitted part). */
#define PIN_NOR_CS          GPIO_NUM_10
#define PIN_NOR_MOSI        GPIO_NUM_11   /* IO0 */
#define PIN_NOR_CLK         GPIO_NUM_12
#define PIN_NOR_MISO        GPIO_NUM_13   /* IO1 */
#define PIN_NOR_WP          GPIO_NUM_14   /* IO2 */
#define PIN_NOR_HOLD        GPIO_NUM_9    /* IO3 */

/* Optional I2C display. */
#define PIN_I2C_SDA         GPIO_NUM_47
#define PIN_I2C_SCL         GPIO_NUM_21

/* Active 5 V buzzer via BC817 (also native USB D+). */
#define PIN_BUZZER          GPIO_NUM_20

/* External status LEDs (not fitted yet). Note: the on-board RGB LED of
 * this DevKitC clone is on GPIO48 (v1.0 layout), shared with LED_STATUS. */
#define PIN_LED_ACTIVITY    GPIO_NUM_38
#define PIN_LED_STATUS      GPIO_NUM_48
