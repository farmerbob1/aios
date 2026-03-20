/* AIOS v2 — ChaosFS Inode Cache
 * 16-entry LRU cache with dirty write-back on eviction. */

#include "chaos_types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"
#include "../../kernel/heap.h"

/* Extern from chaos_block.c */
extern int chaos_block_read(uint32_t block_idx, void* buffer);
extern int chaos_block_write(uint32_t block_idx, const void* buffer);

/* Extern from chaos_alloc.c */
extern uint32_t chaos_get_inode_table_start(void);

/* ── Cache ─────────────────────────────────────────── */

#define INODE_CACHE_SIZE 16

struct inode_cache_entry {
    uint32_t inode_num;   /* 0 = empty slot */
    bool     dirty;
    uint32_t last_used;   /* tick count for LRU */
    struct chaos_inode inode;
};

static struct inode_cache_entry cache[INODE_CACHE_SIZE];

/* ── Helpers ───────────────────────────────────────── */

static void inode_disk_location(uint32_t inode_num, uint32_t* block_idx, uint32_t* offset) {
    uint32_t table_start = chaos_get_inode_table_start();
    *block_idx = table_start + inode_num / CHAOS_INODES_PER_BLOCK;
    *offset = (inode_num % CHAOS_INODES_PER_BLOCK) * CHAOS_INODE_SIZE;
}

static int flush_entry(struct inode_cache_entry* e) {
    if (!e->dirty || e->inode_num == 0) return CHAOS_OK;

    uint32_t blk, off;
    inode_disk_location(e->inode_num, &blk, &off);

    uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return CHAOS_ERR_IO;

    int r = chaos_block_read(blk, buf);
    if (r != CHAOS_OK) { kfree(buf); return r; }

    /* Zero runtime-only fields before writing to disk */
    struct chaos_inode disk_copy = e->inode;
    disk_copy.open_count = 0;
    disk_copy.unlink_pending = false;

    memcpy(buf + off, &disk_copy, CHAOS_INODE_SIZE);
    r = chaos_block_write(blk, buf);
    kfree(buf);

    if (r == CHAOS_OK) e->dirty = false;
    return r;
}

static struct inode_cache_entry* find_lru(void) {
    struct inode_cache_entry* lru = NULL;
    uint32_t oldest = 0xFFFFFFFF;

    /* Prefer empty slots first */
    for (int i = 0; i < INODE_CACHE_SIZE; i++) {
        if (cache[i].inode_num == 0) return &cache[i];
    }

    /* Find least recently used */
    for (int i = 0; i < INODE_CACHE_SIZE; i++) {
        if (cache[i].last_used < oldest) {
            oldest = cache[i].last_used;
            lru = &cache[i];
        }
    }
    return lru;
}

/* ── Public API ────────────────────────────────────── */

void chaos_inode_init(void) {
    memset(cache, 0, sizeof(cache));
}

/* Remove a specific inode from the cache WITHOUT writing it back.
 * Used after chaos_free_inode() to prevent the cache from overwriting
 * the zeroed on-disk slot with stale data on flush/eviction. */
void chaos_inode_evict(uint32_t inode_num) {
    for (int i = 0; i < INODE_CACHE_SIZE; i++) {
        if (cache[i].inode_num == inode_num) {
            cache[i].inode_num = 0;
            cache[i].dirty = false;
            return;
        }
    }
}

int chaos_inode_read(uint32_t inode_num, struct chaos_inode* out) {
    if (inode_num == CHAOS_INODE_NULL || !out) return CHAOS_ERR_INVALID;

    /* Check cache first */
    for (int i = 0; i < INODE_CACHE_SIZE; i++) {
        if (cache[i].inode_num == inode_num) {
            cache[i].last_used = (uint32_t)timer_get_ticks();
            *out = cache[i].inode;
            return CHAOS_OK;
        }
    }

    /* Cache miss — read from disk */
    uint32_t blk, off;
    inode_disk_location(inode_num, &blk, &off);

    uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return CHAOS_ERR_IO;

    int r = chaos_block_read(blk, buf);
    if (r != CHAOS_OK) { kfree(buf); return r; }

    struct chaos_inode loaded;
    memcpy(&loaded, buf + off, CHAOS_INODE_SIZE);
    kfree(buf);

    /* Zero runtime-only fields */
    loaded.open_count = 0;
    loaded.unlink_pending = false;

    /* Evict LRU entry if needed */
    struct inode_cache_entry* slot = find_lru();
    if (slot->inode_num != 0 && slot->dirty) {
        flush_entry(slot);
    }

    slot->inode_num = inode_num;
    slot->inode = loaded;
    slot->dirty = false;
    slot->last_used = (uint32_t)timer_get_ticks();

    *out = loaded;
    return CHAOS_OK;
}

int chaos_inode_write(uint32_t inode_num, const struct chaos_inode* ino) {
    if (inode_num == CHAOS_INODE_NULL || !ino) return CHAOS_ERR_INVALID;

    /* Update cache */
    for (int i = 0; i < INODE_CACHE_SIZE; i++) {
        if (cache[i].inode_num == inode_num) {
            cache[i].inode = *ino;
            cache[i].dirty = true;
            cache[i].last_used = (uint32_t)timer_get_ticks();
            return CHAOS_OK;
        }
    }

    /* Not in cache — add it */
    struct inode_cache_entry* slot = find_lru();
    if (slot->inode_num != 0 && slot->dirty) {
        flush_entry(slot);
    }

    slot->inode_num = inode_num;
    slot->inode = *ino;
    slot->dirty = true;
    slot->last_used = (uint32_t)timer_get_ticks();
    return CHAOS_OK;
}

int chaos_inode_flush(void) {
    int errors = 0;
    for (int i = 0; i < INODE_CACHE_SIZE; i++) {
        if (cache[i].inode_num != 0 && cache[i].dirty) {
            if (flush_entry(&cache[i]) != CHAOS_OK) errors++;
        }
    }
    return errors;
}

void chaos_inode_invalidate(void) {
    memset(cache, 0, sizeof(cache));
}

int chaos_inode_write_through(uint32_t inode_num, const struct chaos_inode* ino) {
    /* Write to cache AND immediately to disk */
    chaos_inode_write(inode_num, ino);

    uint32_t blk, off;
    inode_disk_location(inode_num, &blk, &off);

    uint8_t* buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!buf) return CHAOS_ERR_IO;

    int r = chaos_block_read(blk, buf);
    if (r != CHAOS_OK) { kfree(buf); return r; }

    struct chaos_inode disk_copy = *ino;
    disk_copy.open_count = 0;
    disk_copy.unlink_pending = false;
    memcpy(buf + off, &disk_copy, CHAOS_INODE_SIZE);

    r = chaos_block_write(blk, buf);
    kfree(buf);

    /* Mark cache entry clean since we just wrote it */
    for (int i = 0; i < INODE_CACHE_SIZE; i++) {
        if (cache[i].inode_num == inode_num) {
            cache[i].dirty = false;
            break;
        }
    }

    return r;
}
