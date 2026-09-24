/* Host stub of the ROM CRC: same result as zlib crc32() (verified on the
 * target: CRYSTAL_CASTLES.ST gives 42ce7eed with both). */
#pragma once
#include <stdint.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
        }
    }
    return ~crc;
}
