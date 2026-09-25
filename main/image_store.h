/*
 * RadioFloppy image library on the external SPI flash: images stored as
 * ordered lists of logical blocks (flash_layout.h), described by a catalog
 * in block 0 (two copies). Format: docs/FLASH_LAYOUT.md.
 *
 * Users see images (id, sequence, name), never blocks. The image id never
 * changes; the sequence only sets the order in lists and never moves data.
 *
 * Storing an image (image_store_save): validate, pick free blocks (not
 * necessarily contiguous), erase and program only those blocks, read the
 * image back through the block list and check its CRC-32, and only then
 * write the catalog with the record VALID. An interrupted save leaves no
 * valid record, and its blocks are free again.
 *
 * Replacing an image: the old record is first marked INCOMPLETE (its
 * blocks are free from then on, so replacing also works on a nearly full
 * flash), the old blocks are reused first, and the record becomes VALID
 * again only after the new image has been verified. If replacing fails the
 * old image is lost, but a half-written image is never offered as valid.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "flash_layout.h"

#define IMG_NAME_LEN        52      /* title, NUL terminated if shorter */
#define IMG_TITLE_SIZE      (IMG_NAME_LEN + 1)
#define IMG_MAX_RECORDS     ((int)RF_MAX_DATA_BLOCKS)

/* Record status. 0xFF = erased = unused record. */
#define IMG_FREE            0xff
#define IMG_VALID           0x01
#define IMG_INCOMPLETE      0x02    /* replacing failed/interrupted: no data */

#define IMG_FMT_ST          0x01    /* image format */
#define IMG_STORE_RAW       0x00    /* storage format; compression later */

typedef struct __attribute__((packed)) {
    uint16_t id;                /* 1..65535, never reused while stored */
    uint16_t sequence;          /* display order, 1..n */
    uint8_t status;             /* IMG_* */
    uint8_t format;             /* IMG_FMT_* */
    uint8_t storage_format;     /* IMG_STORE_* */
    uint8_t block_count;        /* blocks in use (0 when INCOMPLETE) */
    uint32_t original_size;     /* image bytes */
    uint32_t stored_size;       /* bytes in the blocks (RAW: == original) */
    uint32_t crc32;             /* CRC-32 (IEEE, as zlib) of the image */
    uint8_t blocks[RF_MAX_BLOCKS_PER_IMAGE];    /* in image byte order */
    char name[IMG_NAME_LEN];
} image_record_t;

_Static_assert(sizeof(image_record_t) == 96, "image record layout");

typedef enum {
    STORE_VALID,                /* catalog loaded */
    STORE_BLANK,                /* never initialised (both copies erased) */
    STORE_OLD_FORMAT,           /* earlier RadioFloppy format: format first */
    STORE_INVALID,              /* unknown/corrupt data or other geometry: not touched */
    STORE_NO_FLASH,             /* external flash missing */
} store_state_t;

typedef struct {
    uint16_t blocks_total;
    uint16_t blocks_used;
    uint16_t blocks_free;
    uint16_t images;            /* valid images */
    uint8_t used_percent;       /* blocks_used / blocks_total, rounded */
} store_usage_t;

/*
 * Read the catalog (both copies, newest valid wins) for a flash of
 * `capacity` bytes. A blank flash is initialised with an empty catalog;
 * an old format or unknown data is left alone.
 */
store_state_t image_store_open(uint32_t capacity);
store_state_t image_store_state(void);
const char *image_store_state_name(store_state_t st);
const rf_geometry_t *image_store_geometry(void);
uint32_t image_store_generation(void);

/* Empty catalog: all images are gone. Needed after STORE_OLD_FORMAT. */
esp_err_t image_store_format(void);

/* Copies of the records (valid and incomplete) sorted by sequence;
 * returns the count. Any task. */
int image_store_list(image_record_t *out, int max);

/* Copy of the record with this id; false if there is none. Any task. */
bool image_store_get(uint16_t id, image_record_t *out);

/* true for a VALID record. */
bool image_store_is_valid(const image_record_t *r);

/* Title of a record as a C string. */
void image_store_title(const image_record_t *r, char out[IMG_TITLE_SIZE]);

/* Id of the first valid image with this title (in sequence order), or 0. */
uint16_t image_store_find_name(const char *name);

void image_store_usage(store_usage_t *u);

/* Blocks an image of `size` bytes needs. */
uint32_t image_store_blocks_needed(uint32_t size);

/* Would a new image (replace_id 0) or a replacement fit? */
bool image_store_fits(uint32_t size, uint16_t replace_id);

/*
 * Store an image (see the header comment). replace_id 0 = new image at the
 * end of the order; otherwise that image is replaced and keeps its id and
 * sequence. *id_out: the image id. Errors: ESP_ERR_INVALID_SIZE (0 bytes
 * or > RF_MAX_IMAGE_SIZE), ESP_ERR_NO_MEM (not enough free blocks or
 * records), ESP_ERR_NOT_FOUND (replace_id unknown), ESP_ERR_INVALID_CRC
 * (read-back mismatch), flash errors.
 */
esp_err_t image_store_save(uint16_t replace_id, const char *name, uint8_t format,
                           const uint8_t *data, uint32_t size, uint16_t *id_out);

/* Remove an image; its blocks are free at once (erased when reused). */
esp_err_t image_store_delete(uint16_t id);

/* Move an image to position 1..n in the order (sequence numbers are
 * renumbered 1..n); no image data is touched. */
esp_err_t image_store_set_position(uint16_t id, int position);

/*
 * The image reader: bytes [offset, offset+len) of a valid image, across
 * its blocks. The only code that turns image offsets into flash addresses.
 */
esp_err_t image_store_read(const image_record_t *r, uint32_t offset, void *buf, uint32_t len);

/* Whole image into buf (original_size bytes), CRC-32 checked. */
esp_err_t image_store_load(uint16_t id, uint8_t *buf);

uint32_t image_store_crc32(uint32_t crc, const void *data, uint32_t len);
