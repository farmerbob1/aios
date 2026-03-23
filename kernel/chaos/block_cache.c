/* AIOS v2 — Block Cache for ChaosFS
 * 512-entry LRU write-through cache with O(1) hash lookup.
 * Sits between ChaosFS block I/O and the ATA driver. */

#include "block_cache.h"
#include "chaos_types.h"
#include "../pmm.h"
#include "../vmm.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"

/* Forward declaration — raw (uncached) block I/O in chaos_block.c */
extern int chaos_block_read_raw(uint32_t block_idx, void* buffer);
extern int chaos_block_write_raw(uint32_t block_idx, const void* buffer);

/* ── Configuration ──────────────────────────────────── */

#define BLOCK_CACHE_SIZE     512
#define BLOCK_CACHE_INVALID  0xFFFFFFFF

/* ── Cache entry ────────────────────────────────────── */

typedef struct {
    uint32_t block_idx;
    uint8_t* data;
    uint32_t lru_counter;
    bool     valid;
} cache_entry_t;

static cache_entry_t cache[BLOCK_CACHE_SIZE];
static uint32_t lru_clock = 0;
static cache_stats_t cache_stats;
static bool cache_enabled = false;

/* ── Hash table (open addressing) ───────────────────── */

#define CACHE_HASH_SIZE   1024  /* power of 2, 50% load factor */
#define CACHE_HASH_EMPTY  0xFFFF

static uint16_t cache_hash[CACHE_HASH_SIZE];

static inline uint32_t cache_hash_fn(uint32_t block_idx) {
    return (block_idx * 2654435761u) >> (32 - 10);  /* >> 22 for 1024 slots */
}

static int cache_lookup(uint32_t block_idx) {
    uint32_t h = cache_hash_fn(block_idx);
    for (uint32_t i = 0; i < CACHE_HASH_SIZE; i++) {
        uint32_t slot = (h + i) & (CACHE_HASH_SIZE - 1);
        if (cache_hash[slot] == CACHE_HASH_EMPTY) return -1;
        uint16_t ci = cache_hash[slot];
        if (cache[ci].block_idx == block_idx && cache[ci].valid) return (int)ci;
    }
    return -1;
}

static void cache_hash_insert(uint32_t block_idx, uint16_t cache_index) {
    uint32_t h = cache_hash_fn(block_idx);
    for (uint32_t i = 0; i < CACHE_HASH_SIZE; i++) {
        uint32_t slot = (h + i) & (CACHE_HASH_SIZE - 1);
        if (cache_hash[slot] == CACHE_HASH_EMPTY) {
            cache_hash[slot] = cache_index;
            return;
        }
    }
    /* Should never happen at 50% load factor */
    serial_printf("[Cache] FATAL: hash table full\n");
}

static void cache_hash_remove(uint32_t block_idx) {
    uint32_t h = cache_hash_fn(block_idx);
    for (uint32_t i = 0; i < CACHE_HASH_SIZE; i++) {
        uint32_t slot = (h + i) & (CACHE_HASH_SIZE - 1);
        if (cache_hash[slot] == CACHE_HASH_EMPTY) return;
        uint16_t ci = cache_hash[slot];
        if (cache[ci].block_idx == block_idx) {
            /* Backward-shift deletion for open addressing */
            uint32_t empty = slot;
            for (uint32_t j = 1; j < CACHE_HASH_SIZE; j++) {
                uint32_t next = (slot + j) & (CACHE_HASH_SIZE - 1);
                if (cache_hash[next] == CACHE_HASH_EMPTY) break;
                uint32_t ideal = cache_hash_fn(cache[cache_hash[next]].block_idx);
                /* Check if 'next' entry belongs at or before 'empty' */
                bool should_move;
                if (empty <= next) {
                    should_move = (ideal <= empty) || (ideal > next);
                } else {
                    should_move = (ideal <= empty) && (ideal > next);
                }
                if (should_move) {
                    cache_hash[empty] = cache_hash[next];
                    empty = next;
                }
            }
            cache_hash[empty] = CACHE_HASH_EMPTY;
            return;
        }
    }
}

/* ── LRU eviction ───────────────────────────────────── */

static int cache_find_lru(void) {
    int best = -1;
    uint32_t best_lru = 0xFFFFFFFF;
    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        if (!cache[i].valid) return i;  /* empty slot */
        if (cache[i].lru_counter < best_lru) {
            best_lru = cache[i].lru_counter;
            best = i;
        }
    }
    return best;
}

static inline void cache_touch(int slot) {
    cache[slot].lru_counter = ++lru_clock;
}

/* ── Init ───────────────────────────────────────────── */

init_result_t block_cache_init(void) {
    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        uint32_t page = pmm_alloc_page();
        if (page == 0) {
            /* OOM — free what we allocated and run without cache */
            for (int j = 0; j < i; j++) {
                pmm_free_page((uint32_t)cache[j].data);
            }
            serial_printf("[Cache] OOM after %d entries, running uncached\n", i);
            return INIT_FAIL;
        }
        vmm_map_page(page, page, PTE_PRESENT | PTE_WRITABLE);
        cache[i].data = (uint8_t*)page;
        cache[i].block_idx = BLOCK_CACHE_INVALID;
        cache[i].valid = false;
        cache[i].lru_counter = 0;
    }

    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        cache_hash[i] = CACHE_HASH_EMPTY;
    }

    memset(&cache_stats, 0, sizeof(cache_stats));
    cache_enabled = true;
    serial_printf("[Cache] Block cache ready: %d entries (%d KB)\n",
                  BLOCK_CACHE_SIZE, BLOCK_CACHE_SIZE * 4);
    return INIT_OK;
}

/* ── Public API ─────────────────────────────────────── */

bool block_cache_is_enabled(void) {
    return cache_enabled;
}

int block_cache_read(uint32_t block_idx, void* buffer) {
    /* Check cache first */
    int slot = cache_lookup(block_idx);
    if (slot >= 0) {
        memcpy(buffer, cache[slot].data, CHAOS_BLOCK_SIZE);
        cache_touch(slot);
        cache_stats.hits++;
        return 0;
    }

    /* Cache miss — read from disk */
    cache_stats.misses++;
    int ret = chaos_block_read_raw(block_idx, buffer);
    if (ret != 0) return ret;

    /* Populate cache */
    slot = cache_find_lru();
    if (slot >= 0) {
        if (cache[slot].valid) {
            cache_hash_remove(cache[slot].block_idx);
            cache_stats.evictions++;
        }
        memcpy(cache[slot].data, buffer, CHAOS_BLOCK_SIZE);
        cache[slot].block_idx = block_idx;
        cache[slot].valid = true;
        cache_touch(slot);
        cache_hash_insert(block_idx, (uint16_t)slot);
    }

    return 0;
}

int block_cache_write(uint32_t block_idx, const void* buffer) {
    /* Always write to disk first (write-through) */
    int ret = chaos_block_write_raw(block_idx, buffer);
    if (ret != 0) return ret;
    cache_stats.write_throughs++;

    /* Update cache if block is cached, or write-allocate if not */
    int slot = cache_lookup(block_idx);
    if (slot >= 0) {
        memcpy(cache[slot].data, buffer, CHAOS_BLOCK_SIZE);
        cache_touch(slot);
    } else {
        slot = cache_find_lru();
        if (slot >= 0) {
            if (cache[slot].valid) {
                cache_hash_remove(cache[slot].block_idx);
                cache_stats.evictions++;
            }
            memcpy(cache[slot].data, buffer, CHAOS_BLOCK_SIZE);
            cache[slot].block_idx = block_idx;
            cache[slot].valid = true;
            cache_touch(slot);
            cache_hash_insert(block_idx, (uint16_t)slot);
        }
    }

    return 0;
}

void block_cache_invalidate(uint32_t block_idx) {
    int slot = cache_lookup(block_idx);
    if (slot >= 0) {
        cache_hash_remove(block_idx);
        cache[slot].valid = false;
        cache[slot].block_idx = BLOCK_CACHE_INVALID;
    }
}

void block_cache_flush(void) {
    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        cache[i].valid = false;
        cache[i].block_idx = BLOCK_CACHE_INVALID;
    }
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        cache_hash[i] = CACHE_HASH_EMPTY;
    }
}

cache_stats_t block_cache_get_stats(void) {
    return cache_stats;
}

int block_cache_hit_rate(void) {
    uint32_t total = cache_stats.hits + cache_stats.misses;
    if (total == 0) return 0;
    return (int)((cache_stats.hits * 100) / total);
}
