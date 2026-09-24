/* Host stub of ESP-IDF esp_heap_caps.h. */
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_DMA      0
#define MALLOC_CAP_8BIT     0
#define MALLOC_CAP_SPIRAM   0
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
