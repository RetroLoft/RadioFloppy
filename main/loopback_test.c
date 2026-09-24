/*
 * FloppyEmulator-ESP32S3 - Shugart GPIO loopback test.
 *
 * With a jumper between one Shugart output (driven through the ULN2003A)
 * and one Shugart input (read through the SN74LVC245A) on J1, this test
 * finds out which output is connected to which input.
 *
 * For every output, REPEATS times:
 *   released -> read inputs (expect HIGH)
 *   asserted -> read inputs (expect LOW, ULN2003A pulls the line to GND)
 *   released -> read inputs (expect HIGH again)
 * An input is only reported as connected when it followed the output on
 * both levels in every repetition. Returning HIGH requires an external
 * pull-up on the Shugart line: there are none on the PCB.
 *
 * Every read also checks that the ESP32 pin is actually driven by the
 * SN74LVC245A: the pin is sampled with the internal pull-up and with the
 * internal pull-down; a driven pin reads the same both times.
 *
 * Only one output is ever active; all outputs are released between steps.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#include "board_pins.h"
#include "tests.h"

#define REPEATS             5   /* measurements per output per pass */
#define SETTLE_MS           2   /* after changing an output */
#define PULL_SETTLE_US      20  /* after changing an input pull */
#define PASS_PAUSE_MS       20  /* between full scan passes */
#define MISSED_PASSES_GONE  2   /* passes without a hit before "removed" */

typedef struct {
    const char *name;
    gpio_num_t gpio;
    int shugart_pin;
} signal_t;

static const signal_t outputs[] = {
    { "INDEX",  PIN_FDD_INDEX,  8  },
    { "TRACK0", PIN_FDD_TRK0,   26 },
    { "WPROT",  PIN_FDD_WPROT,  28 },
    { "RDATA",  PIN_FDD_RDATA,  30 },
    { "READY",  PIN_FDD_READY,  34 },
    { "DSKCHG", PIN_FDD_DSKCHG, 2  },
};

static const signal_t inputs[] = {
    { "DS0",    PIN_FDD_DS0,    10 },
    { "DS1",    PIN_FDD_DS1,    12 },
    { "MOTOR",  PIN_FDD_MOTOR,  16 },
    { "DIR",    PIN_FDD_DIR,    18 },
    { "STEP",   PIN_FDD_STEP,   20 },
    { "WDATA",  PIN_FDD_WDATA,  22 },
    { "WGATE",  PIN_FDD_WGATE,  24 },
    { "SIDE",   PIN_FDD_SIDE,   32 },
};

#define NUM_OUTPUTS (sizeof(outputs) / sizeof(outputs[0]))
#define NUM_INPUTS  (sizeof(inputs) / sizeof(inputs[0]))

/* Per output/input pair: currently reported as connected, and how many
 * consecutive passes it was not seen. */
static bool connected[NUM_OUTPUTS][NUM_INPUTS];
static uint8_t missed[NUM_OUTPUTS][NUM_INPUTS];

static void release_all_outputs(void)
{
    for (size_t o = 0; o < NUM_OUTPUTS; o++) {
        gpio_set_level(outputs[o].gpio, 0);
    }
}

static void pins_init(void)
{
    uint64_t out_mask = 0, in_mask = 0;

    for (size_t o = 0; o < NUM_OUTPUTS; o++) {
        out_mask |= 1ULL << outputs[o].gpio;
    }
    for (size_t i = 0; i < NUM_INPUTS; i++) {
        in_mask |= 1ULL << inputs[i].gpio;
    }

    /* Latch LOW (= released) before the output drivers are enabled. */
    release_all_outputs();
    const gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    const gpio_config_t in_cfg = {
        .pin_bit_mask = in_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));
}

/*
 * Read all inputs. Bit i of *high is set when input i reads HIGH, bit i of
 * *undriven when the pin followed the internal pull-up/pull-down, i.e. the
 * SN74LVC245A is not driving it.
 */
static void read_inputs(uint8_t *high, uint8_t *undriven)
{
    uint8_t with_pu = 0, with_pd = 0;

    for (size_t i = 0; i < NUM_INPUTS; i++) {
        gpio_set_pull_mode(inputs[i].gpio, GPIO_PULLUP_ONLY);
    }
    esp_rom_delay_us(PULL_SETTLE_US);
    for (size_t i = 0; i < NUM_INPUTS; i++) {
        with_pu |= gpio_get_level(inputs[i].gpio) << i;
    }

    for (size_t i = 0; i < NUM_INPUTS; i++) {
        gpio_set_pull_mode(inputs[i].gpio, GPIO_PULLDOWN_ONLY);
    }
    esp_rom_delay_us(PULL_SETTLE_US);
    for (size_t i = 0; i < NUM_INPUTS; i++) {
        with_pd |= gpio_get_level(inputs[i].gpio) << i;
    }

    for (size_t i = 0; i < NUM_INPUTS; i++) {
        gpio_set_pull_mode(inputs[i].gpio, GPIO_FLOATING);
    }

    *high = with_pu & with_pd;
    *undriven = with_pu ^ with_pd;
}

/* Returns a bit mask of the inputs that followed output o on both levels
 * in every repetition. */
static uint8_t scan_output(size_t o)
{
    uint8_t follows = 0xFF;

    for (int r = 0; r < REPEATS && follows; r++) {
        uint8_t hi_before, hi_active, hi_after;
        uint8_t und_before, und_active, und_after;

        release_all_outputs();
        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
        read_inputs(&hi_before, &und_before);

        gpio_set_level(outputs[o].gpio, 1);     /* line pulled to GND */
        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
        read_inputs(&hi_active, &und_active);

        gpio_set_level(outputs[o].gpio, 0);     /* line released */
        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
        read_inputs(&hi_after, &und_after);

        follows &= hi_before & (uint8_t)~hi_active & hi_after;
        follows &= (uint8_t)~(und_before | und_active | und_after);
    }

    release_all_outputs();
    return follows;
}

static void report_connection(size_t o, size_t i)
{
    printf("\nCONNECTION DETECTED!\n\n");
    printf("Output: %s\n", outputs[o].name);
    printf("ESP32:  GPIO%d\n", outputs[o].gpio);
    printf("Shugart pin: %d\n\n", outputs[o].shugart_pin);
    printf("Input:  %s\n", inputs[i].name);
    printf("ESP32:  GPIO%d\n", inputs[i].gpio);
    printf("Shugart pin: %d\n\n", inputs[i].shugart_pin);
    printf("Result: PASS\n");
}

static void report_removed(size_t o, size_t i)
{
    printf("\nConnection removed: %s (pin %d) -> %s (pin %d)\n",
           outputs[o].name, outputs[o].shugart_pin,
           inputs[i].name, inputs[i].shugart_pin);
}

/* One-off overview of the idle input levels, to help diagnose missing
 * pull-ups or an unpowered SN74LVC245A. */
static void report_idle_inputs(void)
{
    uint8_t high, undriven;

    release_all_outputs();
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    read_inputs(&high, &undriven);

    printf("Idle input levels (all outputs released):\n");
    for (size_t i = 0; i < NUM_INPUTS; i++) {
        const char *state = (undriven & (1 << i)) ? "NOT DRIVEN"
                          : (high & (1 << i))     ? "HIGH"
                                                  : "LOW";
        printf("  %-6s GPIO%-2d pin %-2d : %s\n", inputs[i].name,
               inputs[i].gpio, inputs[i].shugart_pin, state);
    }
    if (undriven) {
        printf("WARNING: some inputs are not driven by the SN74LVC245A "
               "(check U3 supply and GND, pin 10).\n");
    }
    printf("\n");
}

void loopback_test_run(void)
{
    pins_init();

    printf("\n=================================\n");
    printf("Floppy Emulator - GPIO Loopback\n");
    printf("=================================\n\n");
    printf("Testing %d outputs and %d inputs\n", (int)NUM_OUTPUTS, (int)NUM_INPUTS);
    printf("Connect one output to one input\n\n");

    report_idle_inputs();

    printf("Scanning...\n");

    while (true) {
        for (size_t o = 0; o < NUM_OUTPUTS; o++) {
            uint8_t follows = scan_output(o);

            for (size_t i = 0; i < NUM_INPUTS; i++) {
                if (follows & (1 << i)) {
                    missed[o][i] = 0;
                    if (!connected[o][i]) {
                        connected[o][i] = true;
                        report_connection(o, i);
                    }
                } else if (connected[o][i] &&
                           ++missed[o][i] >= MISSED_PASSES_GONE) {
                    connected[o][i] = false;
                    report_removed(o, i);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PASS_PAUSE_MS));
    }
}
