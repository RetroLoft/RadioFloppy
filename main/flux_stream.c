/*
 * Read-data flux stream and INDEX pulse via RMT. See flux_stream.h.
 *
 * Each flux transition becomes one RMT symbol: LOW until the transition,
 * then a FLUX_PULSE_TICKS HIGH pulse (GPIO HIGH = ULN2003A pulls /RDATA
 * low), so the leading edge of every /RDATA pulse is exactly on the bitcell
 * grid. A revolution is exactly 100000 * 2 us = 200 ms of symbols, equal to
 * the INDEX loop period, so both channels stay locked.
 *
 * The simple-encoder callback runs in the RMT/DMA ISR. It reads the raw
 * track (PSRAM) of the current cylinder and the live SIDE input for each
 * chunk, so a STEP or SIDE change reaches the pin within about one DMA
 * half buffer (FLUX_DMA_SYMBOLS / 2 transitions, ~1.4 ms).
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "esp_attr.h"

#include "board_pins.h"
#include "disk_image.h"
#include "drive_emu.h"
#include "flux_stream.h"

#define FLUX_DMA_SYMBOLS    512     /* DMA ping-pong buffer, internal RAM */
#define INDEX_MEM_SYMBOLS   48      /* one RMT memory block, no DMA */
#define RMT_MAX_DURATION    32767

/* Pin parked as plain GPIO driving its (LOW) output latch. */
#define GPIO_PARKED_SEL     (SIG_GPIO_OUT_IDX | GPIO_FUNC0_OEN_SEL_M)

static uint32_t rdata_rmt_sel;      /* GPIO matrix value with RMT connected */
static uint32_t index_rmt_sel;

static uint32_t cell_pos;           /* rotational position, 0..MFM_TRACK_CELLS-1 */
static uint32_t cells_since_flux;
static volatile uint32_t revolutions;

static rmt_symbol_word_t index_symbols[INDEX_MEM_SYMBOLS];
static const uint8_t dummy_payload = 0;

void IRAM_ATTR flux_gate(bool rdata, bool index)
{
    REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * PIN_FDD_RDATA,
              rdata ? rdata_rmt_sel : GPIO_PARKED_SEL);
    REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * PIN_FDD_INDEX,
              index ? index_rmt_sel : GPIO_PARKED_SEL);
}

uint32_t flux_revolutions(void)
{
    return revolutions;
}

/* Simple-encoder callback: endless flux stream (never sets *done). */
static size_t IRAM_ATTR flux_encode(const void *data, size_t data_size,
                                    size_t symbols_written, size_t symbols_free,
                                    rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    const uint8_t *raw = disk_track_raw(drive_cylinder(),
                                        gpio_ll_get_level(&GPIO, PIN_FDD_SIDE) ? 0 : 1);
    uint32_t pos = cell_pos;
    uint32_t dist = cells_since_flux;
    size_t n = 0;

    while (n < symbols_free) {
        if (++pos == MFM_TRACK_CELLS) {
            pos = 0;
            revolutions++;
        }
        dist++;
        /* dist >= 2: never two transitions 2 us apart, even right after a
         * track switch joins two different bitstreams. */
        if ((raw[pos >> 3] & (0x80 >> (pos & 7))) && dist >= 2) {
            symbols[n++] = (rmt_symbol_word_t) {
                .level0 = 0, .duration0 = dist * FLUX_CELL_TICKS - FLUX_PULSE_TICKS,
                .level1 = 1, .duration1 = FLUX_PULSE_TICKS,
            };
            dist = 0;
        }
    }

    cell_pos = pos;
    cells_since_flux = dist;
    return n;
}

/* INDEX: HIGH (= /INDEX low) for INDEX_PULSE_MS, then LOW, 200 ms total. */
static size_t build_index_symbols(void)
{
    uint32_t high = INDEX_PULSE_MS * (FLUX_RESOLUTION_HZ / 1000);
    uint32_t total = (uint32_t)MFM_TRACK_CELLS * FLUX_CELL_TICKS;
    uint32_t parts[2 * INDEX_MEM_SYMBOLS];
    uint8_t levels[2 * INDEX_MEM_SYMBOLS];
    size_t np = 0;

    for (int level = 1; level >= 0; level--) {
        uint32_t left = level ? high : total - high;
        while (left) {
            uint32_t d = left > RMT_MAX_DURATION ? RMT_MAX_DURATION : left;
            parts[np] = d;
            levels[np++] = level;
            left -= d;
        }
    }
    if (np & 1) {
        /* Split the last LOW part so the parts pair up into symbols. */
        parts[np] = parts[np - 1] / 2;
        parts[np - 1] -= parts[np];
        levels[np] = 0;
        np++;
    }

    for (size_t i = 0; i < np / 2; i++) {
        index_symbols[i] = (rmt_symbol_word_t) {
            .level0 = levels[2 * i],     .duration0 = parts[2 * i],
            .level1 = levels[2 * i + 1], .duration1 = parts[2 * i + 1],
        };
    }
    return np / 2;
}

esp_err_t flux_stream_init(void)
{
    rmt_channel_handle_t data_chan, index_chan;
    rmt_encoder_handle_t data_enc, index_enc;

    const rmt_tx_channel_config_t data_cfg = {
        .gpio_num = PIN_FDD_RDATA,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = FLUX_RESOLUTION_HZ,
        .mem_block_symbols = FLUX_DMA_SYMBOLS,
        .trans_queue_depth = 1,
        .flags.with_dma = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&data_cfg, &data_chan);
    if (err != ESP_OK) {
        printf("ERROR: RMT data channel: %s\n", esp_err_to_name(err));
        return err;
    }
    rdata_rmt_sel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * PIN_FDD_RDATA);

    const rmt_tx_channel_config_t index_cfg = {
        .gpio_num = PIN_FDD_INDEX,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = FLUX_RESOLUTION_HZ,
        .mem_block_symbols = INDEX_MEM_SYMBOLS,
        .trans_queue_depth = 1,
    };
    err = rmt_new_tx_channel(&index_cfg, &index_chan);
    if (err != ESP_OK) {
        printf("ERROR: RMT index channel: %s\n", esp_err_to_name(err));
        return err;
    }
    index_rmt_sel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * PIN_FDD_INDEX);

    /* Park both pins right away: LOW latch, output enabled, GPIO driven. */
    gpio_set_level(PIN_FDD_RDATA, 0);
    gpio_set_level(PIN_FDD_INDEX, 0);
    gpio_ll_output_enable(&GPIO, PIN_FDD_RDATA);
    gpio_ll_output_enable(&GPIO, PIN_FDD_INDEX);
    flux_gate(false, false);

    const rmt_simple_encoder_config_t data_enc_cfg = {
        .callback = flux_encode,
        .min_chunk_size = 1,
    };
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&data_enc_cfg, &data_enc));
    const rmt_copy_encoder_config_t index_enc_cfg = { };
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&index_enc_cfg, &index_enc));

    ESP_ERROR_CHECK(rmt_enable(data_chan));
    ESP_ERROR_CHECK(rmt_enable(index_chan));

    /* Start both channels at the same moment: INDEX at the track start. */
    rmt_channel_handle_t chans[] = { data_chan, index_chan };
    const rmt_sync_manager_config_t sync_cfg = {
        .tx_channel_array = chans,
        .array_size = 2,
    };
    rmt_sync_manager_handle_t sync;
    ESP_ERROR_CHECK(rmt_new_sync_manager(&sync_cfg, &sync));

    size_t n_index = build_index_symbols();
    const rmt_transmit_config_t index_tx = { .loop_count = -1 };
    ESP_ERROR_CHECK(rmt_transmit(index_chan, index_enc, index_symbols,
                                 n_index * sizeof(rmt_symbol_word_t), &index_tx));
    const rmt_transmit_config_t data_tx = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(data_chan, data_enc, &dummy_payload,
                                 sizeof(dummy_payload), &data_tx));

    /* Both channels now run forever. The hardware start synchronisation
     * (group-wide tx_sim_en) is no longer needed and must not affect the
     * other RMT channels (status LED), so remove it again. */
    ESP_ERROR_CHECK(rmt_del_sync_manager(sync));

    return ESP_OK;
}
