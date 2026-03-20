/* AIOS v2 — Virtual Memory Manager (Phase 1)
 * 4KB page tables, selective identity mapping. */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

#define PTE_PRESENT      0x001
#define PTE_WRITABLE     0x002
#define PTE_USER         0x004
#define PTE_WRITETHROUGH 0x008
#define PTE_NOCACHE      0x010
#define PTE_ACCESSED     0x020
#define PTE_DIRTY        0x040

init_result_t vmm_init(struct boot_info* info);
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void vmm_map_range(uint32_t virt_start, uint32_t phys_start, uint32_t size, uint32_t flags);
void vmm_unmap_page(uint32_t virt);
void vmm_set_flags(uint32_t virt, uint32_t flags);
void vmm_flush_tlb(uint32_t virt);
void vmm_flush_tlb_all(void);
