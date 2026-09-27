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
