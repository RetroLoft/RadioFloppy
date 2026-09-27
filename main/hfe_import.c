/*
 * Storing an HFE upload as a stream. See hfe_import.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "hfe_import.h"
#include "image_store.h"
#include "img_codec.h"

#define PIECE_MAX   8192        /* larger pieces are passed on in parts */

struct hfe_import {
    uint32_t size;
    uint32_t received;
    uint32_t crc;               /* of the file, as received */
    uint32_t buffer_bytes;
    hfe_import_error_t err;     /* sticky */
    hfe_result_t result;        /* of the HFE check */
    hfe_info_t info;
    image_writer_t *w;
    img_deflate_stream_t *z;
    esp_err_t out_err;          /* of the writer, seen from the compressor */
    uint8_t *region;            /* HFE_REGION_MAX, for the checks */
    hfe_stream_t check;
    hfe_pack_t pack;            /* counting only */
    uint8_t *piece[2];          /* alternating copies for the compressor */
    int cur;
    /* Verification */
    uint32_t verify_crc;
    uint32_t verify_len;
};

const char *hfe_import_error_name(hfe_import_error_t e)
{
    switch (e) {
    case HFE_IMPORT_OK:                 return "ok";
    case HFE_IMPORT_FORMAT:             return "not a usable HFE file";
    case HFE_IMPORT_STORED_TOO_LARGE:   return "too large, also compressed";
    case HFE_IMPORT_NO_SPACE:           return "no free storage";
    case HFE_IMPORT_NO_MEMORY:          return "no memory";
    case HFE_IMPORT_VERIFY:             return "stored data did not read back correctly";
    case HFE_IMPORT_BUSY:               return "another image is being written";
    default:                            return "flash error";
    }
}

static hfe_import_error_t writer_error(esp_err_t err)
{
    return err == ESP_ERR_INVALID_SIZE ? HFE_IMPORT_STORED_TOO_LARGE
         : err == ESP_ERR_NO_MEM ? HFE_IMPORT_NO_SPACE : HFE_IMPORT_FLASH;
}

/* Compressed bytes -> flash (called by the compressor's worker). */
static esp_err_t to_writer(void *ctx, const uint8_t *data, size_t len)
{
    hfe_import_t *imp = ctx;
    esp_err_t err = image_store_writer_write(imp->w, data, (uint32_t)len);
    if (err != ESP_OK) {
        imp->out_err = err;
    }
    return err;
}

static void release(hfe_import_t *imp)
{
    if (imp->z) {
        img_deflate_close(imp->z, true, NULL);
        imp->z = NULL;
    }
    image_store_writer_abort(imp->w);
    imp->w = NULL;
}

static void destroy(hfe_import_t *imp)
{
    release(imp);
    free(imp->region);
    free(imp->piece[0]);
    free(imp->piece[1]);
    free(imp);
}

static hfe_import_error_t fail(hfe_import_t *imp, hfe_import_error_t e)
{
    if (imp->err == HFE_IMPORT_OK) {
        imp->err = e;
    }
    release(imp);           /* the reserved blocks are free again at once */
    return imp->err;
}

hfe_import_error_t hfe_import_begin(uint32_t size, uint32_t buffer_bytes, hfe_import_t **impp)
{
    hfe_import_t *imp = calloc(1, sizeof(*imp));

    *impp = NULL;
    if (!imp) {
        return HFE_IMPORT_NO_MEMORY;
    }
    imp->size = size;
    imp->buffer_bytes = buffer_bytes;
    imp->region = heap_caps_malloc(HFE_REGION_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    imp->piece[0] = heap_caps_malloc(PIECE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    imp->piece[1] = heap_caps_malloc(PIECE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!imp->region || !imp->piece[0] || !imp->piece[1]) {
        destroy(imp);
        return HFE_IMPORT_NO_MEMORY;
    }
    esp_err_t err = image_store_writer_open(&imp->w);
    if (err != ESP_OK) {
        destroy(imp);
        return err == ESP_ERR_NO_MEM ? HFE_IMPORT_NO_MEMORY
             : err == ESP_ERR_INVALID_STATE ? HFE_IMPORT_BUSY : HFE_IMPORT_FLASH;
    }
    if (img_deflate_open(to_writer, imp, &imp->z) != ESP_OK) {
        destroy(imp);
        return HFE_IMPORT_NO_MEMORY;
    }
    hfe_pack_init(&imp->pack, NULL, buffer_bytes);
    hfe_stream_init(&imp->check, size, imp->region, &imp->pack);
    *impp = imp;
    return HFE_IMPORT_OK;
}

hfe_import_error_t hfe_import_write(hfe_import_t *imp, const uint8_t *data, uint32_t len)
{
    while (len && imp->err == HFE_IMPORT_OK) {
        uint32_t n = len < PIECE_MAX ? len : PIECE_MAX;
        if (n > imp->size - imp->received) {
            return fail(imp, HFE_IMPORT_FORMAT);        /* more than announced */
        }
        imp->crc = image_store_crc32(imp->crc, data, n);
        imp->received += n;
        if ((imp->result = hfe_stream_feed(&imp->check, data, n)) != HFE_OK) {
            imp->info = *hfe_stream_info(&imp->check);
            return fail(imp, HFE_IMPORT_FORMAT);
        }
        /* The compressor works on a copy while the caller receives the next piece. */
        uint8_t *copy = imp->piece[imp->cur];
        imp->cur ^= 1;
        memcpy(copy, data, n);
        if (img_deflate_write(imp->z, copy, n) != ESP_OK) {
            return fail(imp, imp->out_err != ESP_OK ? writer_error(imp->out_err)
                                                    : HFE_IMPORT_NO_MEMORY);
        }
        data += n;
        len -= n;
    }
    return imp->err;
}

/* ---- Verification: decompress from the flash, check again ------------------ */

static esp_err_t from_writer(void *ctx, uint32_t off, uint8_t *buf, size_t len)
{
    hfe_import_t *imp = ctx;
    return image_store_writer_read(imp->w, off, buf, (uint32_t)len);
}

static esp_err_t to_check(void *ctx, const uint8_t *data, size_t len)
{
    hfe_import_t *imp = ctx;
    if (len > imp->size - imp->verify_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    imp->verify_crc = image_store_crc32(imp->verify_crc, data, len);
    imp->verify_len += (uint32_t)len;
    return hfe_stream_feed(&imp->check, data, (uint32_t)len) == HFE_OK ? ESP_OK : ESP_FAIL;
}

hfe_import_error_t hfe_import_end(hfe_import_t *imp, uint32_t *crc)
{
    if (imp->err != HFE_IMPORT_OK) {
        return imp->err;
    }
    *crc = imp->crc;
    if (imp->received != imp->size) {
        return fail(imp, HFE_IMPORT_FORMAT);
    }
    /* The raw data as a whole: complete, and its tracks fit. */
    imp->result = hfe_stream_end(&imp->check);
    imp->info = *hfe_stream_info(&imp->check);
    if (imp->result != HFE_OK) {
        return fail(imp, HFE_IMPORT_FORMAT);
    }
    size_t stored = 0;
    esp_err_t err = img_deflate_close(imp->z, false, &stored);
    imp->z = NULL;
    if (err != ESP_OK) {
        return fail(imp, imp->out_err != ESP_OK ? writer_error(imp->out_err) : HFE_IMPORT_NO_MEMORY);
    }
    if ((err = image_store_writer_finish(imp->w)) != ESP_OK) {
        return fail(imp, writer_error(err));
    }

    /* What is on the flash, decompressed, must be the same file again. */
    hfe_info_t first = imp->info;
    hfe_pack_init(&imp->pack, NULL, imp->buffer_bytes);
    hfe_stream_init(&imp->check, imp->size, imp->region, &imp->pack);
    size_t total = 0;
    err = img_inflate_stream(stored, from_writer, imp, to_check, imp, &total);
    hfe_result_t r = hfe_stream_end(&imp->check);
    if (err != ESP_OK || r != HFE_OK || total != imp->size || imp->verify_len != imp->size ||
        imp->verify_crc != imp->crc || hfe_stream_info(&imp->check)->bytes_needed != first.bytes_needed) {
        printf("HFE import: verification failed (%s, %s, %lu of %lu bytes, CRC %08lx/%08lx)\n",
               err == ESP_OK ? "inflate ok" : "inflate failed", hfe_result_name(r),
               (unsigned long)total, (unsigned long)imp->size, (unsigned long)imp->verify_crc,
               (unsigned long)imp->crc);
        return fail(imp, err == ESP_ERR_NO_MEM ? HFE_IMPORT_NO_MEMORY : HFE_IMPORT_VERIFY);
    }
    return HFE_IMPORT_OK;
}

hfe_import_error_t hfe_import_commit(hfe_import_t *imp, uint16_t replace_id, const char *name,
                                     uint16_t *id_out)
{
    hfe_import_error_t e = imp->err;

    if (e == HFE_IMPORT_OK) {
        uint8_t fmt = imp->info.version == 3 ? IMG_FMT_HFE3 : IMG_FMT_HFE;
        esp_err_t err = image_store_writer_commit(imp->w, replace_id, name, fmt, IMG_STORE_DEFLATE,
                                                  imp->size, imp->crc, id_out);
        imp->w = NULL;                  /* the commit freed it */
        if (err != ESP_OK) {
            e = err == ESP_ERR_NOT_FOUND ? HFE_IMPORT_FLASH
              : err == ESP_ERR_NO_MEM ? HFE_IMPORT_NO_SPACE
              : err == ESP_ERR_INVALID_CRC ? HFE_IMPORT_VERIFY : HFE_IMPORT_FLASH;
        }
    }
    destroy(imp);
    return e;
}

void hfe_import_abort(hfe_import_t *imp)
{
    if (imp) {
        destroy(imp);
    }
}

const hfe_info_t *hfe_import_info(const hfe_import_t *imp)
{
    return &imp->info;
}

hfe_result_t hfe_import_result(const hfe_import_t *imp)
{
    return imp->result;
}

uint32_t hfe_import_stored(const hfe_import_t *imp)
{
    return imp->w ? image_store_writer_size(imp->w) : 0;
}
