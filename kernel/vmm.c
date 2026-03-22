/* AIOS v2 — Virtual Memory Manager
 * x86 two-level paging: page directory (1024 entries) -> page tables (1024 each).
 * Selective identity mapping (virt == phys) of only needed regions.
 * All page tables pre-allocated before paging is enabled. */

#include "vmm.h"
#include "pmm.h"
#include "../include/string.h"
#include "../drivers/serial.h"
#include "panic.h"
#include "../include/kaos/export.h"

/* ── Address decomposition ──────────────────────────────── */
#define PD_INDEX(v)    ((v) >> 22)
#define PT_INDEX(v)    (((v) >> 12) & 0x3FF)

/* ── State ──────────────────────────────────────────────── */
static uint32_t* page_directory;
static uint32_t  pt_phys_addrs[1024];  /* tracks allocated PT physical addresses */
static bool      paging_enabled = false;

/* ── Ensure page table exists for a given PD index ──────── */
static uint32_t* ensure_page_table(uint32_t pd_idx) {
    if (page_directory[pd_idx] & PTE_PRESENT) {
        return (uint32_t*)(page_directory[pd_idx] & 0xFFFFF000);
    }

    if (paging_enabled) {
        /* After paging, we should never need a new PT — all pre-allocated */
        serial_printf("[VMM] ERROR: need PT for PD[%u] after paging enabled\n", pd_idx);
        kernel_panic("VMM: page table not pre-allocated");
        return 0;
    }

    /* Allocate new page table */
    uint32_t pt_phys = pmm_alloc_page();
    if (pt_phys == 0) {
        kernel_panic("VMM: out of memory for page table");
        return 0;
    }

    memset((void*)pt_phys, 0, PAGE_SIZE);
    page_directory[pd_idx] = pt_phys | PTE_PRESENT | PTE_WRITABLE;
    pt_phys_addrs[pd_idx] = pt_phys;
    return (uint32_t*)pt_phys;
}

/* ── Public API ─────────────────────────────────────────── */

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_idx = PD_INDEX(virt);
    uint32_t pt_idx = PT_INDEX(virt);

    uint32_t* pt = ensure_page_table(pd_idx);
    pt[pt_idx] = (phys & 0xFFFFF000) | flags | PTE_PRESENT;

    if (paging_enabled) {
        vmm_flush_tlb(virt);
    }
}

void vmm_map_range(uint32_t virt_start, uint32_t phys_start,
                   uint32_t size, uint32_t flags) {
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        vmm_map_page(virt_start + off, phys_start + off, flags);
    }
}

void vmm_unmap_page(uint32_t virt) {
    uint32_t pd_idx = PD_INDEX(virt);
    uint32_t pt_idx = PT_INDEX(virt);

    if (!(page_directory[pd_idx] & PTE_PRESENT)) return;

    uint32_t* pt = (uint32_t*)(page_directory[pd_idx] & 0xFFFFF000);
    pt[pt_idx] = 0;

    if (paging_enabled) {
        vmm_flush_tlb(virt);
    }
}

void vmm_set_flags(uint32_t virt, uint32_t flags) {
    uint32_t pd_idx = PD_INDEX(virt);
    uint32_t pt_idx = PT_INDEX(virt);

    if (!(page_directory[pd_idx] & PTE_PRESENT)) return;

    uint32_t* pt = (uint32_t*)(page_directory[pd_idx] & 0xFFFFF000);
    pt[pt_idx] = (pt[pt_idx] & 0xFFFFF000) | flags | PTE_PRESENT;

    if (paging_enabled) {
        vmm_flush_tlb(virt);
    }
}

void vmm_flush_tlb(uint32_t virt) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_flush_tlb_all(void) {
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* ── Init ───────────────────────────────────────────────── */
init_result_t vmm_init(struct boot_info* info) {
    /* Step 1: Allocate page directory */
    uint32_t pd_phys = pmm_alloc_page();
    if (pd_phys == 0) return INIT_FATAL;
    page_directory = (uint32_t*)pd_phys;
    memset(page_directory, 0, PAGE_SIZE);
    memset(pt_phys_addrs, 0, sizeof(pt_phys_addrs));

    serial_printf("[VMM] page directory at 0x%08x\n", pd_phys);

    /* Step 2: Pre-allocate ALL 1024 page tables for full 4GB address space.
     * Cost: 1024 * 4KB = 4MB (1.6% of 256MB).
     * Benefit: vmm_map_page() can never panic post-paging.
     * Required for PCI MMIO mapping (E1000 BAR0 at ~0xFEB00000, etc). */
    serial_printf("[VMM] pre-allocating all 1024 page tables (4MB)\n");

    for (uint32_t i = 0; i < 1024; i++) {
        ensure_page_table(i);
    }

    /* Step 3: Map low 1MB (256 pages) */
    for (uint32_t addr = 0; addr < 0x100000; addr += PAGE_SIZE) {
        uint32_t flags = PTE_PRESENT | PTE_WRITABLE;
        /* VGA region gets NOCACHE | WRITETHROUGH */
        if (addr >= 0xA0000 && addr < 0xC0000) {
            flags |= PTE_NOCACHE | PTE_WRITETHROUGH;
        }
        vmm_map_page(addr, addr, flags);
    }

    /* Step 4: Map kernel segments */
    for (uint32_t s = 0; s < info->kernel_segment_count; s++) {
        vmm_map_range(info->kernel_segments[s].phys_start,
                      info->kernel_segments[s].phys_start,
                      info->kernel_segments[s].phys_end - info->kernel_segments[s].phys_start,
                      PTE_PRESENT | PTE_WRITABLE);
    }

    /* Step 5: Map PMM bitmap */
    uint32_t bm_addr = pmm_get_bitmap_addr();
    uint32_t bm_size = pmm_get_bitmap_size();
    vmm_map_range(bm_addr, bm_addr, bm_size, PTE_PRESENT | PTE_WRITABLE);

    /* Step 6: Map framebuffer */
    if (info->fb_addr != 0) {
        uint32_t fb_size = info->fb_pitch * info->fb_height;
        vmm_map_range(info->fb_addr, info->fb_addr, fb_size,
                      PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHROUGH);
        serial_printf("[VMM] framebuffer mapped: 0x%08x (%u bytes)\n", info->fb_addr, fb_size);
    }

    /* Step 7: Map page directory itself */
    vmm_map_page(pd_phys, pd_phys, PTE_PRESENT | PTE_WRITABLE);

    /* Step 8: Map all allocated page tables */
    for (uint32_t i = 0; i < 1024; i++) {
        if (pt_phys_addrs[i] != 0) {
            vmm_map_page(pt_phys_addrs[i], pt_phys_addrs[i], PTE_PRESENT | PTE_WRITABLE);
        }
    }

    /* Step 9-10: Enable paging */
    serial_print("[VMM] enabling paging...\n");

    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pd_phys) : "memory");

    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");

    paging_enabled = true;

    serial_print("[VMM] paging enabled\n");
    return INIT_OK;
}

KAOS_EXPORT(vmm_map_page)
KAOS_EXPORT(vmm_map_range)
KAOS_EXPORT(vmm_unmap_page)
