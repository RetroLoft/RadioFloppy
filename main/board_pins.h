/*
 * Pin mapping of the RadioFloppy PCB, revision 2 (hardware/v1.1):
 * ESP32-S3-DevKitC (Otronic, N16R8) on the carrier board.
 *
 * Source: hardware/v1.1/"Floppy emulator.kicad_pcb" (U1 footprint pads and
 * their nets), checked against the user's hardware description on
 * 2026-09-29. All pins match.
 */
#pragma once

#include "driver/gpio.h"

/* Push buttons: switch to GND, use internal pull-up, active low.
 * On revision 1 SW_PREV was physically the RIGHT button and SW_NEXT the
 * LEFT one; check this again on the revision 2 board. */
#define PIN_BTN_PREV        GPIO_NUM_8   /* SW1 / SW_PREV - right */
#define PIN_BTN_NEXT        GPIO_NUM_19  /* SW2 / SW_NEXT - left (also native USB D-) */

/* Console: UART0 via the DevKitC USB-to-UART bridge. */
#define PIN_UART0_TX        GPIO_NUM_43
#define PIN_UART0_RX        GPIO_NUM_44

/* Shugart inputs (Atari -> ESP32) through SN74LVC245A, active low.
 * DS0/DS1 have 10k pull-ups to +5 V on the connector side (R4/R5); the
 * other inputs float while the computer is off. */
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

/* One SPI bus (SPI2) for the NOR flash (U2) and the external SD card
 * module (J5). Each has its own CS; both CS lines must be HIGH whenever
 * the other device is used. */
#define PIN_SPI_MOSI        GPIO_NUM_11
#define PIN_SPI_CLK         GPIO_NUM_12
#define PIN_SPI_MISO        GPIO_NUM_13

/* External SPI NOR flash (schematic: W25Q128JVS; the fitted part on the
 * first boards is an S25FL128L). */
#define PIN_NOR_CS          GPIO_NUM_10
#define PIN_NOR_MOSI        PIN_SPI_MOSI  /* IO0 */
#define PIN_NOR_CLK         PIN_SPI_CLK
#define PIN_NOR_MISO        PIN_SPI_MISO  /* IO1 */
#define PIN_NOR_WP          GPIO_NUM_14   /* IO2 */
#define PIN_NOR_HOLD        GPIO_NUM_9    /* IO3 */

/* External SD card module (J5), SPI mode. */
#define PIN_SD_CS           GPIO_NUM_38

/* I2C display (J3). */
#define PIN_I2C_SDA         GPIO_NUM_47
#define PIN_I2C_SCL         GPIO_NUM_21

/* Active 5 V buzzer via BC817 (1k to the base). GPIO46 is a strapping
 * pin: pulled low at reset, so the buzzer is silent while booting. */
#define PIN_BUZZER          GPIO_NUM_46

/* LEDs on J4, each through 470R, active high. GPIO48 also drives the
 * DevKitC's on-board RGB LED data input; it is used as a plain GPIO, so
 * that LED stays dark. GPIO20 is the native USB D+ pad (see board.c). */
#define PIN_LED_ACTIVITY    GPIO_NUM_48
#define PIN_LED_STATUS      GPIO_NUM_20
