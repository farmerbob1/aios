/* AIOS v2 — Physical Memory Manager
 * Bitmap-based page allocator. Each bit = one 4KB page (0=free, 1=used).
 * Bitmap is dynamically placed in a free E820 region after the kernel. */

#include "pmm.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "../include/kaos/export.h"

/* ── State ──────────────────────────────────────────────── */
static uint8_t* bitmap;
static uint32_t bitmap_size;        /* bytes in bitmap (page-aligned) */
static uint32_t bitmap_phys_addr;   /* physical address of bitmap */
static uint32_t total_pages;
static uint32_t used_pages;
static uint32_t max_phys_addr_val;
static uint32_t next_alloc_hint;    /* bit index for next-fit */

/* ── Bit helpers ────────────────────────────────────────── */
static inline void bitmap_set(uint32_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(uint32_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline bool bitmap_test(uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

/* ── Range helpers ──────────────────────────────────────── */
static void mark_range_used(uint32_t start_addr, uint32_t end_addr) {
    uint32_t start_page = start_addr / PAGE_SIZE;
    uint32_t end_page = (end_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    if (end_page > total_pages) end_page = total_pages;
    for (uint32_t p = start_page; p < end_page; p++)
        bitmap_set(p);
}

static void mark_range_free(uint32_t start_addr, uint32_t end_addr) {
    uint32_t start_page = start_addr / PAGE_SIZE;
    uint32_t end_page = end_addr / PAGE_SIZE;
    if (end_page > total_pages) end_page = total_pages;
    for (uint32_t p = start_page; p < end_page; p++)
        bitmap_clear(p);
}

/* ── Check if a range overlaps with reserved areas ──────── */
static bool range_overlaps(uint32_t start, uint32_t size,
                           uint32_t region_start, uint32_t region_end) {
    uint32_t end = start + size;
    return (start < region_end && end > region_start);
}

/* ── Find placement for bitmap ──────────────────────────── */
static uint32_t find_bitmap_placement(struct boot_info* info, uint32_t needed_size) {
    for (uint32_t i = 0; i < info->e820_count; i++) {
        struct e820_entry* e = &info->e820_entries[i];
        if (e->type != 1) continue; /* only usable regions */

        /* Clamp 64-bit values to 32-bit */
        uint32_t region_base, region_end;
        if (e->base >= 0x100000000ULL) continue;
        region_base = (uint32_t)e->base;

        uint64_t end64 = e->base + e->length;
        if (end64 > 0x100000000ULL) end64 = 0x100000000ULL;
        region_end = (uint32_t)end64;

        /* Try placing at the start of this region, page-aligned up */
        uint32_t candidate = (region_base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        while (candidate + needed_size <= region_end) {
            bool overlap = false;

            /* Check low memory */
            if (range_overlaps(candidate, needed_size, 0, 0x100000)) {
                candidate = 0x100000;
                continue;
            }

            /* Check boot_info page */
            if (range_overlaps(candidate, needed_size, 0x10000, 0x11000)) {
                candidate = 0x11000;
                candidate = (candidate + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                continue;
            }

            /* Check each kernel segment */
            for (uint32_t s = 0; s < info->kernel_segment_count; s++) {
                uint32_t seg_start = info->kernel_segments[s].phys_start;
                uint32_t seg_end = info->kernel_segments[s].phys_end;
                if (range_overlaps(candidate, needed_size, seg_start, seg_end)) {
                    candidate = (seg_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    overlap = true;
                    break;
                }
            }

            if (overlap) continue;

            /* Found a valid placement */
            return candidate;
        }
    }

    return 0; /* failed to place bitmap */
}

/* ── Init ───────────────────────────────────────────────── */
init_result_t pmm_init(struct boot_info* info) {
    max_phys_addr_val = info->max_phys_addr;
    total_pages = max_phys_addr_val / PAGE_SIZE;
    bitmap_size = (total_pages + 7) / 8;
    /* Page-align bitmap size up */
    bitmap_size = (bitmap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    serial_printf("[PMM] total_pages=%u bitmap_size=%u bytes\n", total_pages, bitmap_size);

    /* Find placement for bitmap */
    bitmap_phys_addr = find_bitmap_placement(info, bitmap_size);
    if (bitmap_phys_addr == 0) {
        serial_print("[PMM] ERROR: cannot place bitmap\n");
        return INIT_FATAL;
    }

    bitmap = (uint8_t*)bitmap_phys_addr;
    serial_printf("[PMM] bitmap at 0x%08x (%u bytes)\n", bitmap_phys_addr, bitmap_size);

    /* Step 6: Mark all pages as used */
    memset(bitmap, 0xFF, bitmap_size);

    /* Step 7: Walk E820, mark usable pages as free */
    for (uint32_t i = 0; i < info->e820_count; i++) {
        struct e820_entry* e = &info->e820_entries[i];
        if (e->type != 1) continue;

        /* Clamp to 32-bit range */
        if (e->base >= 0x100000000ULL) continue;
        uint32_t base = (uint32_t)e->base;
        uint64_t end64 = e->base + e->length;
        if (end64 > 0x100000000ULL) end64 = 0x100000000ULL;
        uint32_t end = (uint32_t)end64;

        /* Page-align: base up, end down */
        uint32_t page_base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint32_t page_end = end & ~(PAGE_SIZE - 1);
        if (page_base < page_end) {
            mark_range_free(page_base, page_end);
        }
    }

    /* Step 8: Re-reserve critical regions */
    /* Low memory 0x00000 - 0xFFFFF (256 pages) */
    mark_range_used(0x00000, 0x100000);

    /* Each kernel segment */
    for (uint32_t s = 0; s < info->kernel_segment_count; s++) {
        mark_range_used(info->kernel_segments[s].phys_start,
                        info->kernel_segments[s].phys_end);
    }

    /* Bitmap's own pages */
    mark_range_used(bitmap_phys_addr, bitmap_phys_addr + bitmap_size);

    /* Step 9: Count free pages */
    uint32_t free_count = 0;
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) free_count++;
    }
    used_pages = total_pages - free_count;

    next_alloc_hint = 0;

    serial_printf("[PMM] free=%u used=%u total=%u\n", free_count, used_pages, total_pages);
    return INIT_OK;
}

/* ── Alloc single page ──────────────────────────────────── */
uint32_t pmm_alloc_page(void) {
    /* Next-fit: scan from hint, wrap around */
    uint32_t start = next_alloc_hint;

    for (uint32_t i = 0; i < total_pages; i++) {
        uint32_t bit = (start + i) % total_pages;
        if (!bitmap_test(bit)) {
            bitmap_set(bit);
            used_pages++;
            next_alloc_hint = (bit + 1) % total_pages;
            return bit * PAGE_SIZE;
        }
    }

    return 0; /* out of memory */
}

/* ── Alloc contiguous pages ─────────────────────────────── */
uint32_t pmm_alloc_pages(uint32_t count) {
    if (count == 0) return 0;

    uint32_t run_start = 0;
    uint32_t run_length = 0;

    for (uint32_t bit = 0; bit < total_pages; bit++) {
        if (!bitmap_test(bit)) {
            if (run_length == 0) run_start = bit;
            run_length++;
            if (run_length == count) {
                /* Found — mark all as used */
                for (uint32_t j = run_start; j < run_start + count; j++)
                    bitmap_set(j);
                used_pages += count;
                return run_start * PAGE_SIZE;
            }
        } else {
            run_length = 0;
        }
    }

    return 0; /* not enough contiguous pages */
}

/* ── Free ───────────────────────────────────────────────── */
void pmm_free_page(uint32_t phys_addr) {
    uint32_t bit = phys_addr / PAGE_SIZE;
    if (bit < total_pages && bitmap_test(bit)) {
        bitmap_clear(bit);
        used_pages--;
    }
}

void pmm_free_pages(uint32_t phys_addr, uint32_t count) {
    uint32_t start_bit = phys_addr / PAGE_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bit = start_bit + i;
        if (bit < total_pages && bitmap_test(bit)) {
            bitmap_clear(bit);
            used_pages--;
        }
    }
}

/* ── Stats ──────────────────────────────────────────────── */
uint32_t pmm_get_total_pages(void)    { return total_pages; }
uint32_t pmm_get_free_pages(void)     { return total_pages - used_pages; }
uint32_t pmm_get_used_pages(void)     { return used_pages; }
uint32_t pmm_get_max_phys_addr(void)  { return max_phys_addr_val; }
uint32_t pmm_get_bitmap_addr(void)    { return bitmap_phys_addr; }
uint32_t pmm_get_bitmap_size(void)    { return bitmap_size; }

KAOS_EXPORT(pmm_alloc_page)
KAOS_EXPORT(pmm_alloc_pages)
KAOS_EXPORT(pmm_free_page)
KAOS_EXPORT(pmm_free_pages)
