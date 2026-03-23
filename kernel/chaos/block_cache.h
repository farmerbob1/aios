/* AIOS v2 — Block Cache for ChaosFS
 * LRU write-through cache sitting in the block I/O layer. */

#pragma once

#include "../../include/types.h"
#include "../../include/boot_info.h"

typedef struct {
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t write_throughs;
} cache_stats_t;

init_result_t   block_cache_init(void);
bool            block_cache_is_enabled(void);
void            block_cache_invalidate(uint32_t block_idx);
void            block_cache_flush(void);
cache_stats_t   block_cache_get_stats(void);
int             block_cache_hit_rate(void);

/* Called by chaos_block.c — the cache layer between callers and raw disk I/O */
int block_cache_read(uint32_t block_idx, void* buffer);
int block_cache_write(uint32_t block_idx, const void* buffer);
