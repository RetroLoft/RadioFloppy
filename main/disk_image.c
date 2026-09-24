/*
 * Floppy image source and MFM track generation. Images are raw .ST sector
 * dumps, 80 cylinders, 9 sectors of 512 bytes, one or two sides (derived
 * from the file size). They come from the external SPI flash image store
 * and are loaded into PSRAM; the firmware itself contains no images.
 *
 * At start-up every track is encoded once into PSRAM (80 x 2 x 12500
 * bytes), so a STEP or SIDE change never has to wait for encoding. For a
 * single-sided image, side 1 is an unformatted track. A low-priority
 * background task then decodes every track again and compares it with the
 * image, so a broken encoder shows up in the log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "disk_image.h"
#include "ext_flash.h"
#include "image_store.h"

const char *disk_image_name;
const char *disk_image_source;
int disk_image_heads;
uint8_t *disk_tracks;

static const uint8_t *image_data;

/* .ST order: cylinder, then head, then sectors 1..9. */
static const uint8_t *image_sector_data(int cyl, int head)
{
    return image_data + (size_t)(cyl * disk_image_heads + head) * MFM_SECTORS * MFM_SECTOR_SIZE;
}

static void verify_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    int bad = 0;

    for (int cyl = 0; cyl < DISK_CYLINDERS; cyl++) {
        for (int head = 0; head < disk_image_heads; head++) {
            if (mfm_verify_track(disk_track_raw(cyl, head), image_sector_data(cyl, head),
                                 cyl, head) != MFM_SECTORS) {
                printf("ERROR: MFM track %d side %d failed verification\n", cyl, head);
                bad++;
            }
        }
        vTaskDelay(1);
    }
    printf("MFM verify: %d/%d tracks OK (%lld ms, background)\n",
           DISK_CYLINDERS * disk_image_heads - bad, DISK_CYLINDERS * disk_image_heads,
           (esp_timer_get_time() - t0) / 1000);
    vTaskDelete(NULL);
}

/* Get the raw image from the external flash (image store) into PSRAM. */
static esp_err_t load_external(const uint8_t **data, size_t *size)
{
    esp_err_t err = ext_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    err = image_store_open();
    const rf_catalog_t *cat = image_store_catalog();
    if (cat) {
        int n = 0;
        for (int i = 0; i < RF_CAT_RECORDS; i++) {
            n += cat->rec[i].status == RF_ST_VALID;
        }
        printf("Image catalog: OK (format v%d, generation %lu, %d image%s)\n",
               cat->hdr.version, (unsigned long)cat->hdr.generation, n, n == 1 ? "" : "s");
    } else {
        printf("Image catalog: none found (empty image store)\n");
    }

    const rf_record_t *rec = image_store_find(DISK_EXTERNAL_IMAGE);
    if (!rec) {
        printf("ERROR: image \"%s\" not found in the image store\n", DISK_EXTERNAL_IMAGE);
        return ESP_ERR_NOT_FOUND;
    }
    if (rec->format != RF_FMT_ST) {
        printf("ERROR: image \"%s\" has unsupported format %u\n", rec->name, rec->format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *buf = heap_caps_malloc(rec->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    int64_t t0 = esp_timer_get_time();
    err = image_store_load(rec, buf);
    if (err != ESP_OK) {
        printf("ERROR: reading \"%s\" failed: %s\n", rec->name,
               err == ESP_ERR_INVALID_CRC ? "CRC mismatch" : esp_err_to_name(err));
        free(buf);
        return err;
    }

    printf("Selected image: %s\n", rec->name);
    printf("Source: EXTERNAL SPI FLASH (0x%06lx)\n", (unsigned long)rec->start);
    printf("Image size: %lu bytes\n", (unsigned long)rec->size);
    printf("Image CRC: OK (%08lx, read in %lld ms)\n", (unsigned long)rec->crc32,
           (esp_timer_get_time() - t0) / 1000);

    disk_image_name = rec->name;
    *data = buf;
    *size = rec->size;
    return ESP_OK;
}

esp_err_t disk_image_init(void)
{
    const size_t side_bytes = (size_t)DISK_CYLINDERS * MFM_SECTORS * MFM_SECTOR_SIZE;
    size_t size = 0;

    const uint8_t *data = NULL;
    if (load_external(&data, &size) != ESP_OK) {
        printf("Image not available - drive stays disabled.\n");
        return ESP_ERR_NOT_FOUND;
    }
    image_data = data;
    disk_image_source = "EXTERNAL SPI FLASH";

    if (size == side_bytes) {
        disk_image_heads = 1;
    } else if (size == 2 * side_bytes) {
        disk_image_heads = 2;
    } else {
        printf("ERROR: %s is %u bytes, expected %u or %u (80 cyl, 9 sectors)\n",
               disk_image_name, (unsigned)size, (unsigned)side_bytes, (unsigned)(2 * side_bytes));
        return ESP_ERR_INVALID_SIZE;
    }

    disk_tracks = heap_caps_malloc((size_t)DISK_CYLINDERS * DISK_MAX_HEADS * MFM_TRACK_BYTES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *work = heap_caps_malloc(MFM_TRACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!disk_tracks || !work) {
        printf("ERROR: no memory for the MFM tracks\n");
        return ESP_ERR_NO_MEM;
    }

    /* Encode in internal RAM, then copy the finished track to PSRAM. */
    printf("Generating MFM tracks...\n");
    int64_t t0 = esp_timer_get_time();
    for (int cyl = 0; cyl < DISK_CYLINDERS; cyl++) {
        for (int head = 0; head < DISK_MAX_HEADS; head++) {
            if (head < disk_image_heads) {
                mfm_build_track(work, image_sector_data(cyl, head), cyl, head);
            } else {
                mfm_build_blank_track(work);
            }
            memcpy((uint8_t *)disk_track_raw(cyl, head), work, MFM_TRACK_BYTES);
        }
    }
    free(work);
    printf("MFM tracks: %d generated in %lld ms (verification runs in background)\n",
           DISK_CYLINDERS * DISK_MAX_HEADS, (esp_timer_get_time() - t0) / 1000);

    /* Lowest priority, on the core that does not run the GPIO/RMT ISRs. */
    xTaskCreatePinnedToCore(verify_task, "mfm_verify", 4096, NULL, 1, NULL, 1);
    return ESP_OK;
}
