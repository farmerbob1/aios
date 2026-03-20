/* AIOS v2 — Kernel Heap (Phase 1)
 * Dual allocator: slab (<=2048B) + buddy (>2048B) with page ownership table. */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

typedef enum {
    PAGE_UNUSED = 0,
    PAGE_SLAB,
    PAGE_BUDDY,
    PAGE_RESERVED
} page_owner_t;

/* Exposed for slab.c to set page ownership when allocating new slab pages */
extern uint8_t* page_ownership;

init_result_t heap_init(struct boot_info* info);
void*  kmalloc(size_t size);
void*  kzmalloc(size_t size);
void*  kmalloc_aligned(size_t size, size_t alignment);
void*  krealloc(void* ptr, size_t new_size);
void   kfree(void* ptr);
void   kfree_aligned(void* ptr);
size_t kmalloc_usable_size(void* ptr);
void   heap_mark_reserved(uint32_t phys_addr, uint32_t page_count);
uint32_t heap_get_used(void);
uint32_t heap_get_free(void);
uint32_t heap_get_slab_used(void);
uint32_t heap_get_buddy_used(void);
