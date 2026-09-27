/*
 * Image compression with the ROM miniz. See img_codec.h.
 *
 * The compressor state (about 150 KiB) and the decompressor state live in
 * PSRAM and are allocated per call: compression runs only when an image
 * is stored, decompression when one is loaded.
 *
 * The ROM compressor needs more stack than the HTTP server task has, so it
 * runs in a short-lived task with its own stack (freed afterwards).
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "miniz.h"

#include "img_codec.h"

/* Dictionary probes: 128 is miniz's default (good ratio, a few seconds
 * for a 720 KiB image). Raw deflate, no zlib header: the catalog has the
 * sizes and a CRC-32 of the image. */
#define DEFLATE_FLAGS   TDEFL_DEFAULT_MAX_PROBES

#define DEFLATE_STACK   (24 * 1024)

static size_t deflate_here(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_cap)
{
    tdefl_compressor *c = heap_caps_malloc(sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) {
        return 0;
    }
    size_t out = 0;
    if (tdefl_init(c, NULL, NULL, DEFLATE_FLAGS) == TDEFL_STATUS_OKAY) {
        size_t in_size = n;
        size_t out_size = dst_cap;
        tdefl_status st = tdefl_compress(c, src, &in_size, dst, &out_size, TDEFL_FINISH);
        /* DONE: everything consumed and flushed; anything else: it did not
         * fit in dst_cap, so it is not worth it. */
        if (st == TDEFL_STATUS_DONE && in_size == n && out_size < dst_cap) {
            out = out_size;
        }
    }
    free(c);
    return out;
}

esp_err_t img_inflate(const uint8_t *src, size_t n, uint8_t *dst, size_t out_len)
{
    tinfl_decompressor *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    tinfl_init(d);
    size_t in_size = n;
    size_t out_size = out_len;
    tinfl_status st = tinfl_decompress(d, src, &in_size, dst, dst, &out_size,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(d);
    return st == TINFL_STATUS_DONE && out_size == out_len ? ESP_OK : ESP_ERR_INVALID_CRC;
}

typedef struct {
    const uint8_t *src;
    size_t n;
    uint8_t *dst;
    size_t cap;
    size_t out;
    SemaphoreHandle_t done;
} deflate_job_t;

static void deflate_task(void *arg)
{
    deflate_job_t *job = arg;
    job->out = deflate_here(job->src, job->n, job->dst, job->cap);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

size_t img_deflate(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_cap)
{
    deflate_job_t job = { .src = src, .n = n, .dst = dst, .cap = dst_cap };
    job.done = xSemaphoreCreateBinary();
    if (!job.done) {
        return 0;
    }
    /* Same core and priority as the caller (the HTTP server, core 1). */
    if (xTaskCreatePinnedToCore(deflate_task, "deflate", DEFLATE_STACK, &job,
                                uxTaskPriorityGet(NULL), NULL, 1) != pdPASS) {
        vSemaphoreDelete(job.done);
        return 0;                       /* no memory for the stack: store RAW */
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    return job.out;
}

esp_err_t img_inflate_crc(const uint8_t *src, size_t n, size_t out_len, uint32_t *crc)
{
    tinfl_decompressor *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *dict = heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_ERR_INVALID_CRC;
    size_t in_ofs = 0, dict_ofs = 0, total = 0;
    uint32_t c = 0;

    if (!d || !dict) {
        free(d);
        free(dict);
        return ESP_ERR_NO_MEM;
    }
    tinfl_init(d);
    while (true) {
        size_t in_size = n - in_ofs;
        size_t out_size = TINFL_LZ_DICT_SIZE - dict_ofs;
        tinfl_status st = tinfl_decompress(d, src + in_ofs, &in_size, dict, dict + dict_ofs,
                                           &out_size, 0);   /* wrapping 32 KiB window */
        in_ofs += in_size;
        c = esp_rom_crc32_le(c, dict + dict_ofs, out_size);
        total += out_size;
        dict_ofs = (dict_ofs + out_size) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) {
            err = total == out_len ? ESP_OK : ESP_ERR_INVALID_CRC;
            break;
        }
        if (st < 0 || total > out_len || (st == TINFL_STATUS_NEEDS_MORE_INPUT && in_ofs >= n)) {
            break;
        }
    }
    free(d);
    free(dict);
    *crc = c;
    return err;
}
