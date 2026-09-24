/*
 * Embedded floppy images: raw .ST sector dumps, 80 cylinders, 9 sectors of
 * 512 bytes, one or two sides (derived from the file size).
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

extern const uint8_t retroloft_start[] asm("_binary_RETROLOFT_TEST_720K_ST_start");
extern const uint8_t retroloft_end[] asm("_binary_RETROLOFT_TEST_720K_ST_end");
#if HAVE_CRYSTAL_CASTLES
extern const uint8_t crystal_start[] asm("_binary_CRYSTAL_CASTLES_ST_start");
extern const uint8_t crystal_end[] asm("_binary_CRYSTAL_CASTLES_ST_end");
#endif

typedef struct {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
} embedded_image_t;

#if DISK_IMAGE_SELECT == DISK_IMAGE_CRYSTAL_CASTLES && HAVE_CRYSTAL_CASTLES
static const embedded_image_t image = { "CRYSTAL_CASTLES.ST", crystal_start, crystal_end };
#else
#if DISK_IMAGE_SELECT == DISK_IMAGE_CRYSTAL_CASTLES
#warning "images/CRYSTAL_CASTLES.ST not present: using RETROLOFT_TEST_720K.ST"
#endif
static const embedded_image_t image = { "RETROLOFT_TEST_720K.ST", retroloft_start, retroloft_end };
#endif

const char *disk_image_name;
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

esp_err_t disk_image_init(void)
{
    const size_t side_bytes = (size_t)DISK_CYLINDERS * MFM_SECTORS * MFM_SECTOR_SIZE;
    size_t size = image.end - image.start;

    disk_image_name = image.name;
    image_data = image.start;
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
    printf("MFM tracks: %d encoded in %lld ms (verification runs in background)\n",
           DISK_CYLINDERS * DISK_MAX_HEADS, (esp_timer_get_time() - t0) / 1000);

    /* Lowest priority, on the core that does not run the GPIO/RMT ISRs. */
    xTaskCreatePinnedToCore(verify_task, "mfm_verify", 4096, NULL, 1, NULL, 1);
    return ESP_OK;
}
