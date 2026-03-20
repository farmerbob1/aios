/* AIOS v2 — ChaosFS Block and Inode Allocators
 * Bitmap cache (write-through), next-fit block allocation,
 * inode table scan for allocation. */

#include "chaos_types.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../kernel/heap.h"

/* Extern from chaos_block.c */
extern int chaos_block_read(uint32_t block_idx, void* buffer);
extern int chaos_block_write(uint32_t block_idx, const void* buffer);

/* ── State ─────────────────────────────────────────── */

static uint32_t* bitmap_cache;
static uint32_t  bitmap_cache_words;
static uint32_t  next_alloc_hint;
static uint32_t  total_blocks_val;
static uint32_t  bitmap_start_block;
static uint32_t  bitmap_block_count;
static uint32_t  data_start_val;
static uint32_t  inode_table_start_val;
static uint32_t  inode_table_block_count;
static uint32_t  total_inodes_val;
static uint32_t  free_blocks_count;
static uint32_t  free_inodes_count;

/* ── Init / Shutdown ───────────────────────────────── */

int chaos_alloc_init(const struct chaos_superblock* sb) {
    total_blocks_val = sb->total_blocks;
    bitmap_start_block = sb->bitmap_start;
    bitmap_block_count = sb->bitmap_blocks;
    data_start_val = sb->data_start;
    inode_table_start_val = sb->inode_table_start;
    inode_table_block_count = sb->inode_table_blocks;
    total_inodes_val = sb->total_inodes;
    free_blocks_count = sb->free_blocks;
    free_inodes_count = sb->free_inodes;

    bitmap_cache_words = (total_blocks_val + 31) / 32;
    uint32_t cache_bytes = bitmap_cache_words * 4;
    bitmap_cache = (uint32_t*)kmalloc(cache_bytes);
    if (!bitmap_cache) return CHAOS_ERR_NO_SPACE;

    /* Load bitmap from disk */
    uint8_t* block_buf = (uint8_t*)bitmap_cache;
    for (uint32_t i = 0; i < bitmap_block_count; i++) {
        int r = chaos_block_read(bitmap_start_block + i,
                                  block_buf + i * CHAOS_BLOCK_SIZE);
        if (r != CHAOS_OK) { kfree(bitmap_cache); bitmap_cache = NULL; return r; }
    }

    next_alloc_hint = data_start_val / 32;
    return CHAOS_OK;
}

void chaos_alloc_shutdown(void) {
    if (bitmap_cache) { kfree(bitmap_cache); bitmap_cache = NULL; }
}

/* ── Bitmap write-through ──────────────────────────── */

static int bitmap_flush_word(uint32_t word_idx) {
    /* Determine which 4KB bitmap block contains this word */
    uint32_t byte_offset = word_idx * 4;
    uint32_t block_in_bitmap = byte_offset / CHAOS_BLOCK_SIZE;
    /* Write the entire 4KB block */
    uint8_t* block_ptr = (uint8_t*)bitmap_cache + block_in_bitmap * CHAOS_BLOCK_SIZE;
    return chaos_block_write(bitmap_start_block + block_in_bitmap, block_ptr);
}

/* ── Block allocation ──────────────────────────────── */

uint32_t chaos_alloc_block(void) {
    if (!bitmap_cache || free_blocks_count == 0) return CHAOS_BLOCK_NULL;

    uint32_t start = next_alloc_hint;
    uint32_t idx = start;
    do {
        if (bitmap_cache[idx] != 0xFFFFFFFF) {
            /* Found a word with a free bit */
            uint32_t word = bitmap_cache[idx];
            uint32_t bit = 0;
            /* Find first zero bit */
            uint32_t inv = ~word;
            while (!(inv & 1)) { inv >>= 1; bit++; }

            uint32_t block_index = idx * 32 + bit;
            if (block_index >= total_blocks_val) goto next_word;
            if (block_index < data_start_val) goto next_word;

            /* Set bit */
            bitmap_cache[idx] |= (1U << bit);
            bitmap_flush_word(idx);
            next_alloc_hint = idx;
            free_blocks_count--;
            return block_index;
        }
    next_word:
        idx = (idx + 1) % bitmap_cache_words;
    } while (idx != start);

    return CHAOS_BLOCK_NULL;
}

void chaos_free_block(uint32_t block_idx) {
    if (!bitmap_cache || block_idx < data_start_val || block_idx >= total_blocks_val) return;

    uint32_t word_idx = block_idx / 32;
    uint32_t bit = block_idx % 32;

    bitmap_cache[word_idx] &= ~(1U << bit);
    bitmap_flush_word(word_idx);
    free_blocks_count++;

    if (word_idx < next_alloc_hint) {
        next_alloc_hint = word_idx;
    }
}

/* ── Inode allocation ──────────────────────────────── */

uint32_t chaos_alloc_inode(void) {
    if (free_inodes_count == 0) return CHAOS_INODE_NULL;

    uint8_t* block_buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!block_buf) return CHAOS_INODE_NULL;

    for (uint32_t b = 0; b < inode_table_block_count; b++) {
        if (chaos_block_read(inode_table_start_val + b, block_buf) != CHAOS_OK) {
            kfree(block_buf);
            return CHAOS_INODE_NULL;
        }

        for (uint32_t s = 0; s < CHAOS_INODES_PER_BLOCK; s++) {
            uint32_t inode_num = b * CHAOS_INODES_PER_BLOCK + s;
            if (inode_num == 0) continue;  /* slot 0 is reserved (CHAOS_INODE_NULL) */

            struct chaos_inode* ino = (struct chaos_inode*)(block_buf + s * CHAOS_INODE_SIZE);
            if (ino->magic != CHAOS_INODE_MAGIC) {
                /* Free slot found */
                free_inodes_count--;
                kfree(block_buf);
                return inode_num;
            }
        }
    }

    kfree(block_buf);
    return CHAOS_INODE_NULL;
}

void chaos_free_inode(uint32_t inode_num) {
    if (inode_num == CHAOS_INODE_NULL) return;

    uint32_t block_in_table = inode_num / CHAOS_INODES_PER_BLOCK;
    uint32_t slot_in_block = inode_num % CHAOS_INODES_PER_BLOCK;

    uint8_t* block_buf = (uint8_t*)kmalloc(CHAOS_BLOCK_SIZE);
    if (!block_buf) return;

    if (chaos_block_read(inode_table_start_val + block_in_table, block_buf) != CHAOS_OK) {
        kfree(block_buf);
        return;
    }

    /* Zero the entire 128-byte slot (clears magic, marking it free) */
    memset(block_buf + slot_in_block * CHAOS_INODE_SIZE, 0, CHAOS_INODE_SIZE);
    chaos_block_write(inode_table_start_val + block_in_table, block_buf);
    free_inodes_count++;

    kfree(block_buf);
}

/* ── Getters ───────────────────────────────────────── */

uint32_t chaos_get_free_blocks(void)  { return free_blocks_count; }
uint32_t chaos_get_free_inodes(void)  { return free_inodes_count; }
uint32_t chaos_get_total_blocks(void) { return total_blocks_val; }
uint32_t chaos_get_data_start(void)   { return data_start_val; }
uint32_t chaos_get_inode_table_start(void) { return inode_table_start_val; }
uint32_t chaos_get_inode_table_blocks(void) { return inode_table_block_count; }
uint32_t chaos_get_total_inodes(void) { return total_inodes_val; }
uint32_t* chaos_get_bitmap_cache(void) { return bitmap_cache; }
uint32_t chaos_get_bitmap_words(void) { return bitmap_cache_words; }
