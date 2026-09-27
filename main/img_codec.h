/*
 * Image compression for the library: raw deflate (RFC 1951) with the
 * miniz tdefl/tinfl in the ESP32-S3 ROM. The host tests link their own
 * implementation of these two functions.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Compress src[n] into dst (dst_cap bytes). Returns the compressed size,
 * or 0 when the result would not be smaller than dst_cap (store raw). */
size_t img_deflate(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_cap);

/* Decompress src[n] into dst: exactly out_len bytes are expected. */
esp_err_t img_inflate(const uint8_t *src, size_t n, uint8_t *dst, size_t out_len);

/* Decompress src[n] without keeping the result (32 KiB window): its length
 * must be out_len; *crc gets its CRC-32. For checking before storing. */
esp_err_t img_inflate_crc(const uint8_t *src, size_t n, size_t out_len, uint32_t *crc);
