/* AIOS v2 — Kernel Heap
 * Dual allocator: slab (<=2048B) + buddy (>2048B).
 * Routes kfree via page_ownership table (O(1) lookup). */

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "slab.h"
#include "buddy.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "panic.h"

/* ── Page ownership table ───────────────────────────────── */
uint8_t* page_ownership = 0;
static uint32_t ownership_page_count;

/* ── Buddy region tracking ──────────────────────────────── */
static uint32_t buddy_region_start;
static uint32_t buddy_region_size;

/* ── Init ───────────────────────────────────────────────── */
init_result_t heap_init(struct boot_info* info) {
    (void)info;

    /* Step 1-2: Allocate page_ownership table */
    uint32_t total_phys_pages = pmm_get_max_phys_addr() / PAGE_SIZE;
    uint32_t ownership_bytes = total_phys_pages; /* 1 byte per page */
    ownership_page_count = (ownership_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32_t ownership_phys = pmm_alloc_pages(ownership_page_count);
    if (ownership_phys == 0) {
        serial_print("[HEAP] ERROR: cannot allocate page_ownership table\n");
        return INIT_FATAL;
    }

    /* Map and initialize */
    vmm_map_range(ownership_phys, ownership_phys,
                  ownership_page_count * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);
    page_ownership = (uint8_t*)ownership_phys;
    memset(page_ownership, PAGE_UNUSED, ownership_bytes);

    /* Mark ownership table's own pages as reserved */
    heap_mark_reserved(ownership_phys, ownership_page_count);

    serial_printf("[HEAP] page_ownership at 0x%08x (%u pages for %u entries)\n",
                  ownership_phys, ownership_page_count, total_phys_pages);

    /* Step 3-4: Allocate buddy region */
    /* Try 16MB (4096 pages), fall back to smaller */
    static const uint32_t try_sizes[] = { 4096, 2048, 1024, 256, 0 };
    buddy_region_start = 0;
    buddy_region_size = 0;

    for (int i = 0; try_sizes[i] > 0; i++) {
        uint32_t phys = pmm_alloc_pages(try_sizes[i]);
        if (phys != 0) {
            buddy_region_start = phys;
            buddy_region_size = try_sizes[i] * PAGE_SIZE;
            serial_printf("[HEAP] buddy region: %u pages (%u KB) at 0x%08x\n",
                          try_sizes[i], buddy_region_size / 1024, phys);
            break;
        }
    }

    if (buddy_region_start == 0) {
        serial_print("[HEAP] ERROR: cannot allocate buddy region\n");
        return INIT_FATAL;
    }

    /* Map buddy region */
    vmm_map_range(buddy_region_start, buddy_region_start,
                  buddy_region_size, PTE_PRESENT | PTE_WRITABLE);

    /* Mark buddy pages in ownership table */
    uint32_t buddy_page_start = buddy_region_start / PAGE_SIZE;
    uint32_t buddy_page_count = buddy_region_size / PAGE_SIZE;
    for (uint32_t i = 0; i < buddy_page_count; i++) {
        page_ownership[buddy_page_start + i] = PAGE_BUDDY;
    }

    /* Step 5-6: Initialize allocators */
    buddy_init(buddy_region_start, buddy_region_size);
    slab_init();

    serial_print("[HEAP] heap initialized\n");
    return INIT_OK;
}

/* ── kmalloc ────────────────────────────────────────────── */
void* kmalloc(size_t size) {
    if (size == 0) return 0;
    if (size <= 2048) return slab_alloc(size);
    return buddy_alloc(size);
}

void* kzmalloc(size_t size) {
    void* p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

/* ── kfree ──────────────────────────────────────────────── */
void kfree(void* ptr) {
    if (!ptr) return;

    uint32_t page_idx = (uint32_t)ptr >> PAGE_SHIFT;
    uint8_t owner = page_ownership[page_idx];

    switch (owner) {
        case PAGE_SLAB:
            slab_free(ptr);
            break;
        case PAGE_BUDDY:
            buddy_free(ptr);
            break;
        case PAGE_RESERVED:
            kernel_panic("kfree: attempt to free PAGE_RESERVED");
            break;
        case PAGE_UNUSED:
        default:
            kernel_panic("kfree: attempt to free PAGE_UNUSED/invalid");
            break;
    }
}

/* ── kmalloc_usable_size ────────────────────────────────── */
size_t kmalloc_usable_size(void* ptr) {
    if (!ptr) return 0;

    uint32_t page_idx = (uint32_t)ptr >> PAGE_SHIFT;
    uint8_t owner = page_ownership[page_idx];

    if (owner == PAGE_SLAB) {
        /* Slab: usable size = block_size of the slab */
        struct slab_header_view {
            uint16_t block_size;
        };
        struct slab_header_view* hdr =
            (struct slab_header_view*)((uint32_t)ptr & ~(PAGE_SIZE - 1));
        return hdr->block_size;
    }
    if (owner == PAGE_BUDDY) {
        /* Buddy: usable size = 2^order - header */
        struct buddy_hdr_view {
            uint32_t magic;
            uint8_t  order;
        };
        struct buddy_hdr_view* hdr =
            (struct buddy_hdr_view*)((uint8_t*)ptr - BUDDY_HDR_SIZE);
        return (1U << hdr->order) - BUDDY_HDR_SIZE;
    }

    return 0;
}

/* ── krealloc ───────────────────────────────────────────── */
void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return 0; }

    size_t old_size = kmalloc_usable_size(ptr);
    /* Check if new size fits in current allocation */
    if (new_size <= old_size) {
        uint8_t owner = page_ownership[(uint32_t)ptr >> PAGE_SHIFT];
        /* Stay in same allocator? */
        if (owner == PAGE_SLAB && new_size <= 2048) return ptr;
        if (owner == PAGE_BUDDY && new_size > 2048) return ptr;
    }

    /* Allocate new, copy, free old */
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return 0;

    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    kfree(ptr);
    return new_ptr;
}

/* ── Aligned allocation ─────────────────────────────────── */
void* kmalloc_aligned(size_t size, size_t alignment) {
    if (alignment <= 8) return kmalloc(size);

    if (alignment > PAGE_SIZE) {
        /* Force buddy — buddy blocks of order >= alignment are naturally aligned */
        size_t alloc_size = size > alignment ? size : alignment;
        return buddy_alloc(alloc_size);
    }

    /* Over-allocate and align within */
    size_t total = size + alignment + sizeof(void*);
    void* raw = kmalloc(total);
    if (!raw) return 0;

    uint32_t addr = (uint32_t)raw + sizeof(void*);
    uint32_t aligned = (addr + alignment - 1) & ~(alignment - 1);
    ((void**)aligned)[-1] = raw;
    return (void*)aligned;
}

void kfree_aligned(void* ptr) {
    if (!ptr) return;
    void* raw = ((void**)ptr)[-1];
    kfree(raw);
}

/* ── Reserved marking ───────────────────────────────────── */
void heap_mark_reserved(uint32_t phys_addr, uint32_t page_count) {
    uint32_t start = phys_addr / PAGE_SIZE;
    for (uint32_t i = 0; i < page_count; i++) {
        page_ownership[start + i] = PAGE_RESERVED;
    }
}

/* ── Stats ──────────────────────────────────────────────── */
uint32_t heap_get_used(void) {
    return slab_get_total_alloc() + buddy_get_total_alloc();
}

uint32_t heap_get_free(void) {
    return buddy_get_region_size() - buddy_get_total_alloc();
}

uint32_t heap_get_slab_used(void) {
    return slab_get_total_alloc();
}

uint32_t heap_get_buddy_used(void) {
    return buddy_get_total_alloc();
}
