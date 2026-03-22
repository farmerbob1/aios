/* AIOS v2 — Buddy Allocator (Phase 1)
 * Power-of-2 blocks for large allocations > 2048 bytes. */

#pragma once

#include "../include/types.h"

#define BUDDY_MIN_ORDER  12  /* 2^12 = 4KB minimum block */
#define BUDDY_MAX_ORDER  26  /* 2^26 = 64MB maximum block */
#define BUDDY_LEVELS     (BUDDY_MAX_ORDER - BUDDY_MIN_ORDER + 1)
#define BUDDY_MAGIC      0x42554459  /* 'BUDY' */
#define BUDDY_HDR_SIZE   8

void     buddy_init(uint32_t region_start, uint32_t region_size);
void*    buddy_alloc(size_t size);
void     buddy_free(void* ptr);
uint32_t buddy_get_total_alloc(void);
uint32_t buddy_get_region_size(void);
