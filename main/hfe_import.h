/*
 * Storing an HFE file that arrives in pieces (an upload), without ever
 * having the whole file in memory:
 *
 *   while receiving   CRC-32 of the file; an early check of the raw data
 *                     (hfe_stream, counting only: a bad file is refused
 *                     after its header or first bad track); the data is
 *                     compressed (a worker task) straight into reserved
 *                     flash blocks (image_store writer).
 *   hfe_import_end    the stored data is decompressed from the flash and
 *                     checked again as a whole: HFE format and track
 *                     buffer fit, length and CRC-32 against the upload.
 *   hfe_import_commit only now the image appears in the catalog (VALID).
 *
 * Anything else (an error, an interrupted upload, hfe_import_abort)
 * leaves no image and frees the blocks. HFE images are always stored
 * compressed and are always READ_ONLY.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "hfe.h"

typedef struct hfe_import hfe_import_t;

/* What went wrong (for the API error code). */
typedef enum {
    HFE_IMPORT_OK,
    HFE_IMPORT_FORMAT,          /* the file: hfe_import_result() / info detail */
    HFE_IMPORT_STORED_TOO_LARGE,/* compressed larger than one image's blocks */
    HFE_IMPORT_NO_SPACE,        /* no free block left */
    HFE_IMPORT_NO_MEMORY,
    HFE_IMPORT_VERIFY,          /* stored data does not read back to the file */
    HFE_IMPORT_FLASH,           /* flash or catalog error */
    HFE_IMPORT_BUSY,            /* another image is being written */
} hfe_import_error_t;

/*
 * Start an import of a file of `size` bytes whose tracks must fit
 * buffer_bytes of packed track buffer. *imp NULL on failure.
 */
hfe_import_error_t hfe_import_begin(uint32_t size, uint32_t buffer_bytes, hfe_import_t **imp);

/* The next piece of the file (copied). After an error nothing more is
 * done; the error is returned again. */
hfe_import_error_t hfe_import_write(hfe_import_t *imp, const uint8_t *data, uint32_t len);

/* All data received: finish storing and verify (see above). *crc: of the file. */
hfe_import_error_t hfe_import_end(hfe_import_t *imp, uint32_t *crc);

/*
 * Make it an image (new, or replacing replace_id with the same id and
 * position). Frees imp in any case.
 */
hfe_import_error_t hfe_import_commit(hfe_import_t *imp, uint16_t replace_id, const char *name,
                                     uint16_t *id_out);

/* Abandon: nothing is stored. NULL is fine. */
void hfe_import_abort(hfe_import_t *imp);

/* Details: the HFE check (version, geometry, detail text) and its result. */
const hfe_info_t *hfe_import_info(const hfe_import_t *imp);
hfe_result_t hfe_import_result(const hfe_import_t *imp);

/* Stored (compressed) size so far. */
uint32_t hfe_import_stored(const hfe_import_t *imp);

const char *hfe_import_error_name(hfe_import_error_t e);
