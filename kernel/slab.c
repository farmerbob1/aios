/* AIOS v2 — Slab Allocator
 * O(1) alloc/free for fixed-size blocks <= 2048 bytes.
 * 8 size classes: 16, 32, 64, 128, 256, 512, 1024, 2048.
 * Each slab is one 4KB page with a header + fixed-size blocks.
 * Free list is embedded in blocks (first 2 bytes = next free index). */

#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "../include/string.h"
#include "../drivers/serial.h"

/* ── Size classes ───────────────────────────────────────── */
#define SLAB_SIZES_COUNT 8
static const uint16_t slab_sizes[SLAB_SIZES_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

#define SLAB_FREE_END 0xFFFF  /* sentinel: no more free blocks */

/* ── Slab header at start of each slab page ─────────────── */
struct slab_header {
    uint16_t block_size;
    uint16_t total_blocks;
    uint16_t free_count;
    uint16_t first_free;        /* block index, SLAB_FREE_END = none */
    struct slab_header* next;   /* next slab page in this size class */
};

#define SLAB_HEADER_SIZE sizeof(struct slab_header)

/* Head of slab list for each size class */
static struct slab_header* slab_lists[SLAB_SIZES_COUNT];
static uint32_t slab_total_alloc;  /* bytes currently allocated via slab */

/* ── Helpers ────────────────────────────────────────────── */

/* Find size class index for a given size. Returns -1 if too large. */
static int size_class_index(size_t size) {
    for (int i = 0; i < SLAB_SIZES_COUNT; i++) {
        if (size <= slab_sizes[i]) return i;
    }
    return -1;
}

/* Compute the padded header size (aligned to block_size) */
static uint32_t header_padded_size(uint16_t block_size) {
    return ((SLAB_HEADER_SIZE + block_size - 1) / block_size) * block_size;
}

/* Get pointer to the blocks area of a slab page */
static uint8_t* slab_blocks_start(struct slab_header* slab) {
    return (uint8_t*)slab + header_padded_size(slab->block_size);
}

/* Initialize a new slab page */
static struct slab_header* slab_page_init(uint32_t phys, uint16_t block_size) {
    struct slab_header* hdr = (struct slab_header*)phys;
    uint32_t hdr_pad = header_padded_size(block_size);
    uint16_t total = (PAGE_SIZE - hdr_pad) / block_size;

    hdr->block_size = block_size;
    hdr->total_blocks = total;
    hdr->free_count = total;
    hdr->first_free = 0;
    hdr->next = 0;

    /* Build free list: each free block's first 2 bytes = index of next free */
    uint8_t* blocks = (uint8_t*)phys + hdr_pad;
    for (uint16_t i = 0; i < total; i++) {
        uint16_t* slot = (uint16_t*)(blocks + (uint32_t)i * block_size);
        *slot = (i + 1 < total) ? (i + 1) : SLAB_FREE_END;
    }

    return hdr;
}

/* ── Public API ─────────────────────────────────────────── */

void slab_init(void) {
    memset(slab_lists, 0, sizeof(slab_lists));
    slab_total_alloc = 0;
}

void* slab_alloc(size_t size) {
    int idx = size_class_index(size);
    if (idx < 0) return 0;

    uint16_t block_size = slab_sizes[idx];

    /* Find a slab with free space */
    struct slab_header* slab = slab_lists[idx];
    while (slab && slab->free_count == 0) {
        slab = slab->next;
    }

    if (!slab) {
        /* Allocate a new slab page */
        uint32_t phys = pmm_alloc_page();
        if (phys == 0) return 0;

        /* Map it (identity mapping) */
        vmm_map_page(phys, phys, PTE_PRESENT | PTE_WRITABLE);

        /* Mark ownership */
        page_ownership[phys >> PAGE_SHIFT] = PAGE_SLAB;

        /* Initialize slab */
        slab = slab_page_init(phys, block_size);

        /* Prepend to list */
        slab->next = slab_lists[idx];
        slab_lists[idx] = slab;
    }

    /* Pop from free list */
    uint8_t* blocks = slab_blocks_start(slab);
    uint8_t* block_ptr = blocks + (uint32_t)slab->first_free * block_size;
    uint16_t next_free = *(uint16_t*)block_ptr;
    slab->first_free = next_free;
    slab->free_count--;
    slab_total_alloc += block_size;

    return block_ptr;
}

void slab_free(void* ptr) {
    if (!ptr) return;

    /* Get slab header from page-aligned address */
    struct slab_header* slab = (struct slab_header*)((uint32_t)ptr & ~(PAGE_SIZE - 1));
    uint16_t block_size = slab->block_size;

    /* Compute block index */
    uint8_t* blocks = slab_blocks_start(slab);
    uint32_t offset = (uint8_t*)ptr - blocks;
    uint16_t block_index = offset / block_size;

    /* Push onto free list */
    *(uint16_t*)ptr = slab->first_free;
    slab->first_free = block_index;
    slab->free_count++;
    slab_total_alloc -= block_size;
}

/* ── Stats ──────────────────────────────────────────────── */
uint32_t slab_get_total_alloc(void) { return slab_total_alloc; }
