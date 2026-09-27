/*
 * Image compression for the library: raw deflate (RFC 1951) with the
 * miniz tdefl/tinfl in the ESP32-S3 ROM. The host tests link their own
 * implementation of these two functions.
 */
#pragma once

#include <stdbool.h>
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

/* ---- Streaming (images that are never whole in memory, e.g. HFE) ---------- */

/* Output sink: return ESP_OK to go on, anything else stops the stream
 * (that error is passed on). */
typedef esp_err_t (*img_out_fn)(void *ctx, const uint8_t *data, size_t len);

/* Input source: read len bytes at offset off of the compressed data. */
typedef esp_err_t (*img_read_fn)(void *ctx, uint32_t off, uint8_t *buf, size_t len);

typedef struct img_deflate_stream img_deflate_stream_t;

/*
 * Compress a stream: the compressed bytes go to out (called from a worker
 * task with enough stack for the ROM compressor, on core 1). *s NULL on
 * error (no memory).
 */
esp_err_t img_deflate_open(img_out_fn out, void *ctx, img_deflate_stream_t **s);

/*
 * Compress data[len]. Returns as soon as the worker has taken it over;
 * data must stay unchanged until the next img_deflate_write or
 * img_deflate_close returns (so the caller can fill a second buffer
 * meanwhile). Returns the first error of the stream so far (also one
 * from out).
 */
esp_err_t img_deflate_write(img_deflate_stream_t *s, const uint8_t *data, size_t len);

/*
 * Finish (abort: false) or abandon (true) the stream and free it. On
 * success everything was passed to out and *total (if not NULL) is the
 * compressed size.
 */
esp_err_t img_deflate_close(img_deflate_stream_t *s, bool abort, size_t *total);

/*
 * Decompress in_len bytes that read() delivers, passing the result to
 * out in pieces (at most 32 KiB each). OK only when the compressed data
 * ends exactly there; *out_total gets the decompressed size.
 */
esp_err_t img_inflate_stream(size_t in_len, img_read_fn read, void *rctx, img_out_fn out,
                             void *octx, size_t *out_total);
