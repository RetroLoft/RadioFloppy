#pragma once
#include <stdint.h>
#include "flash_layout.h"
#define MOCK_MAX_ERASES 256
typedef struct { uint32_t addr, len; } mock_erase_t;
extern uint8_t mock_flash[RF_FLASH_SIZE];
extern mock_erase_t mock_erases[MOCK_MAX_ERASES];
extern int mock_erase_count;
extern int mock_program_violations;
void mock_flash_reset(uint8_t fill);
