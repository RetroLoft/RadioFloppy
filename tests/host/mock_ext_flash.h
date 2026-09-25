#pragma once
#include <stdint.h>
#include "flash_layout.h"
#define MOCK_FLASH_MAX      (64u << 20)     /* largest capacity the tests use */
#define MOCK_MAX_ERASES     4096
typedef struct { uint32_t addr, len; } mock_erase_t;
extern uint8_t *mock_flash;                 /* MOCK_FLASH_MAX bytes */
extern uint32_t mock_capacity;              /* what ext_flash_size() reports */
extern mock_erase_t mock_erases[MOCK_MAX_ERASES];
extern int mock_erase_count;
extern int mock_program_violations;         /* programming a 0 bit back to 1 */
extern int mock_out_of_range;               /* accesses beyond mock_capacity */
extern int mock_fail_writes_after;          /* >= 0: that many writes succeed, then all fail */
void mock_flash_reset(uint8_t fill);        /* keeps the capacity */
void mock_flash_setup(uint32_t capacity, uint8_t fill);
