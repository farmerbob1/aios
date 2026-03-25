/* AIOS v2 — Physical Memory Manager (Phase 1)
 * Bitmap-based page allocator. Each bit = one 4KB page. */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define PAGES_PER_BYTE  8

init_result_t pmm_init(struct boot_info* info);
uint32_t pmm_alloc_page(void);
uint32_t pmm_alloc_pages(uint32_t count);
void     pmm_free_page(uint32_t phys_addr);
void     pmm_free_pages(uint32_t phys_addr, uint32_t count);
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_free_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_max_phys_addr(void);
uint32_t pmm_get_bitmap_addr(void);
uint32_t pmm_get_bitmap_size(void);
uint32_t pmm_get_usable_ram_pages(void);
