/* AIOS v2 — Buddy Allocator
 * Power-of-2 block allocator for large allocations (>2048 bytes).
 * Manages a contiguous physical memory region with split/merge.
 * Orders 12 (4KB) through 24 (16MB). */

#include "buddy.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "panic.h"

/* ── Block header (8 bytes, present on ALL blocks) ──────── */
struct buddy_header {
    uint32_t magic;
    uint8_t  order;
    uint8_t  is_free;
    uint16_t reserved;
};

/* ── Free list node (overlaid on free blocks after header) ─ */
struct buddy_free_node {
    struct buddy_free_node* next;
    struct buddy_free_node* prev;
};

/* ── State ──────────────────────────────────────────────── */
static struct buddy_free_node* free_lists[BUDDY_LEVELS];
static uint32_t region_base;
static uint32_t region_end;
static uint32_t buddy_total_alloc;

/* ── Free list helpers ──────────────────────────────────── */
static inline int list_index(uint8_t order) {
    return order - BUDDY_MIN_ORDER;
}

static void free_list_insert(uint32_t block_addr, uint8_t order) {
    struct buddy_header* hdr = (struct buddy_header*)block_addr;
    hdr->magic = BUDDY_MAGIC;
    hdr->order = order;
    hdr->is_free = 1;

    struct buddy_free_node* node = (struct buddy_free_node*)(block_addr + BUDDY_HDR_SIZE);
    int idx = list_index(order);
    node->prev = 0;
    node->next = free_lists[idx];
    if (free_lists[idx]) {
        free_lists[idx]->prev = node;
    }
    free_lists[idx] = node;
}

static void free_list_remove(uint32_t block_addr, uint8_t order) {
    struct buddy_free_node* node = (struct buddy_free_node*)(block_addr + BUDDY_HDR_SIZE);
    int idx = list_index(order);

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        free_lists[idx] = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
}

/* ── Init ───────────────────────────────────────────────── */
void buddy_init(uint32_t start, uint32_t size) {
    region_base = start;
    region_end = start + size;
    buddy_total_alloc = 0;
    memset(free_lists, 0, sizeof(free_lists));

    serial_printf("[BUDDY] region 0x%08x - 0x%08x (%u KB)\n",
                  start, region_end, size / 1024);

    /* Decompose region into largest possible aligned power-of-2 blocks */
    uint32_t addr = start;
    uint32_t remaining = size;

    while (remaining >= (1U << BUDDY_MIN_ORDER)) {
        /* Find largest order that fits and is aligned */
        uint8_t order = BUDDY_MAX_ORDER;
        while (order >= BUDDY_MIN_ORDER) {
            uint32_t block_size = 1U << order;
            /* Must fit in remaining AND address must be aligned to block_size
             * relative to region base */
            uint32_t rel = addr - region_base;
            if (block_size <= remaining && (rel % block_size) == 0) {
                free_list_insert(addr, order);
                addr += block_size;
                remaining -= block_size;
                break;
            }
            order--;
        }
        if (order < BUDDY_MIN_ORDER) break; /* shouldn't happen */
    }
}

/* ── Alloc ──────────────────────────────────────────────── */
void* buddy_alloc(size_t size) {
    if (size == 0) return 0;

    /* Account for header */
    size_t actual = size + BUDDY_HDR_SIZE;

    /* Find minimum order */
    uint8_t order = BUDDY_MIN_ORDER;
    while ((1U << order) < actual) {
        order++;
    }
    if (order > BUDDY_MAX_ORDER) return 0;

    /* Find a free block at this order or higher */
    uint8_t found_order = 0;
    bool found = false;
    for (uint8_t o = order; o <= BUDDY_MAX_ORDER; o++) {
        if (free_lists[list_index(o)] != 0) {
            found_order = o;
            found = true;
            break;
        }
    }
    if (!found) return 0;

    /* Remove block from free list */
    struct buddy_free_node* node = free_lists[list_index(found_order)];
    uint32_t block_addr = (uint32_t)node - BUDDY_HDR_SIZE;
    free_list_remove(block_addr, found_order);

    /* Split down to target order */
    while (found_order > order) {
        found_order--;
        uint32_t buddy_addr = block_addr + (1U << found_order);
        free_list_insert(buddy_addr, found_order);
    }

    /* Mark as allocated */
    struct buddy_header* hdr = (struct buddy_header*)block_addr;
    hdr->magic = BUDDY_MAGIC;
    hdr->order = order;
    hdr->is_free = 0;

    buddy_total_alloc += (1U << order);

    /* Return pointer past header */
    return (void*)(block_addr + BUDDY_HDR_SIZE);
}

/* ── Free ───────────────────────────────────────────────── */
void buddy_free(void* ptr) {
    if (!ptr) return;

    uint32_t block_addr = (uint32_t)ptr - BUDDY_HDR_SIZE;
    struct buddy_header* hdr = (struct buddy_header*)block_addr;

    /* Debug validation */
    if (hdr->magic != BUDDY_MAGIC) {
        kernel_panic("buddy_free: bad magic");
    }
    if (hdr->is_free) {
        kernel_panic("buddy_free: double free");
    }

    uint8_t order = hdr->order;
    buddy_total_alloc -= (1U << order);

    /* Try to merge with buddy */
    while (order < BUDDY_MAX_ORDER) {
        /* Buddy address: XOR with block size, relative to region base */
        uint32_t rel = block_addr - region_base;
        uint32_t buddy_rel = rel ^ (1U << order);
        uint32_t buddy_addr = region_base + buddy_rel;

        /* Check buddy is within region */
        if (buddy_addr < region_base || buddy_addr + (1U << order) > region_end)
            break;

        struct buddy_header* buddy_hdr = (struct buddy_header*)buddy_addr;

        /* Can only merge if buddy is free and at the same order */
        if (buddy_hdr->magic != BUDDY_MAGIC) break;
        if (!buddy_hdr->is_free) break;
        if (buddy_hdr->order != order) break;

        /* Remove buddy from its free list */
        free_list_remove(buddy_addr, order);

        /* Merge: take the lower address */
        if (buddy_addr < block_addr)
            block_addr = buddy_addr;

        order++;
    }

    /* Insert merged block into free list */
    free_list_insert(block_addr, order);
}

/* ── Stats ──────────────────────────────────────────────── */
uint32_t buddy_get_total_alloc(void) { return buddy_total_alloc; }

uint32_t buddy_get_region_size(void) { return region_end - region_base; }
