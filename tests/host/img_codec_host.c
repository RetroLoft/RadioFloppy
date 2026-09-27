/*
 * Host stand-in for main/img_codec.c (the ROM deflate is not available on
 * the host): a simple run-length code with the same contract, so the
 * image store's handling of compressed images can be tested. The real
 * deflate/inflate is checked on the device (every compressed image is
 * inflated and CRC-checked before it is stored).
 */
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"

#include "img_codec.h"

int host_codec_corrupt;         /* tests: make img_inflate fail */

size_t img_deflate(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        size_t run = 1;
        while (i + run < n && src[i + run] == src[i] && run < 255) {
            run++;
        }
        if (o + 2 >= dst_cap) {
            return 0;
        }
        dst[o++] = (uint8_t)run;
        dst[o++] = src[i];
        i += run;
    }
    return o;
}

esp_err_t img_inflate(const uint8_t *src, size_t n, uint8_t *dst, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        if (o + src[i] > out_len) {
            return ESP_ERR_INVALID_CRC;
        }
        memset(dst + o, src[i + 1] ^ (host_codec_corrupt ? 1 : 0), src[i]);
        o += src[i];
    }
    return o == out_len ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t img_inflate_crc(const uint8_t *src, size_t n, size_t out_len, uint32_t *crc)
{
    uint8_t *buf = malloc(out_len);
    esp_err_t err = img_inflate(src, n, buf, out_len);
    *crc = esp_rom_crc32_le(0, buf, out_len);
    free(buf);
    return err;
}

/* ---- Streaming (same RLE code, in arbitrary pieces) -------------------------- */

struct img_deflate_stream {
    img_out_fn out;
    void *ctx;
    int have;                   /* a run is open */
    uint8_t byte;
    size_t run;
    uint8_t buf[61];            /* odd size: pairs straddle out() calls */
    size_t n;
    size_t total;
    esp_err_t err;
};

static void put(img_deflate_stream_t *s, uint8_t b)
{
    if (s->err != ESP_OK) {
        return;
    }
    s->buf[s->n++] = b;
    if (s->n == sizeof(s->buf)) {
        s->err = s->out(s->ctx, s->buf, s->n);
        s->total += s->n;
        s->n = 0;
    }
}

esp_err_t img_deflate_open(img_out_fn out, void *ctx, img_deflate_stream_t **sp)
{
    *sp = calloc(1, sizeof(**sp));
    if (!*sp) {
        return ESP_ERR_NO_MEM;
    }
    (*sp)->out = out;
    (*sp)->ctx = ctx;
    return ESP_OK;
}

esp_err_t img_deflate_write(img_deflate_stream_t *s, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len && s->err == ESP_OK; i++) {
        if (s->have && data[i] == s->byte && s->run < 255) {
            s->run++;
            continue;
        }
        if (s->have) {
            put(s, (uint8_t)s->run);
            put(s, s->byte);
        }
        s->have = 1;
        s->byte = data[i];
        s->run = 1;
    }
    return s->err;
}

esp_err_t img_deflate_close(img_deflate_stream_t *s, bool abort, size_t *total)
{
    esp_err_t err = ESP_OK;
    if (!abort) {
        if (s->have) {
            put(s, (uint8_t)s->run);
            put(s, s->byte);
        }
        if (s->err == ESP_OK && s->n) {
            s->err = s->out(s->ctx, s->buf, s->n);
            s->total += s->n;
        }
        err = s->err;
    }
    if (total) {
        *total = s->total;
    }
    free(s);
    return err;
}

esp_err_t img_inflate_stream(size_t in_len, img_read_fn read, void *rctx, img_out_fn out,
                             void *octx, size_t *out_total)
{
    static uint8_t in[3001], dict[32768];
    size_t total = 0, o = 0;
    int have_run = 0;
    uint8_t run = 0;
    esp_err_t err = ESP_OK;

    for (size_t off = 0; off < in_len && err == ESP_OK;) {
        size_t n = in_len - off < sizeof(in) ? in_len - off : sizeof(in);
        if ((err = read(rctx, (uint32_t)off, in, n)) != ESP_OK) {
            break;
        }
        off += n;
        for (size_t i = 0; i < n && err == ESP_OK; i++) {
            if (!have_run) {
                run = in[i];
                have_run = 1;
                continue;
            }
            have_run = 0;
            for (int k = 0; k < run && err == ESP_OK; k++) {
                dict[o++] = in[i] ^ (host_codec_corrupt ? 1 : 0);
                if (o == sizeof(dict)) {
                    err = out(octx, dict, o);
                    total += o;
                    o = 0;
                }
            }
        }
    }
    if (err == ESP_OK && o) {
        err = out(octx, dict, o);
        total += o;
    }
    if (err == ESP_OK && have_run) {
        err = ESP_ERR_INVALID_CRC;              /* cut short */
    }
    if (out_total) {
        *out_total = total;
    }
    return err;
}
