/* AIOS v2 — Global Descriptor Table with TSS (Phase 2) */

#include "gdt.h"
#include "../include/string.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[5];
static struct gdt_ptr   gdtp;
static struct tss_entry tss;

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran) {
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity  = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

init_result_t gdt_init(void) {
    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);
    /* Kernel code: 0x08 */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);
    /* Kernel data: 0x10 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);

    /* TSS: 0x18 */
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = 0x10;
    tss.esp0 = 0x90000;
    tss.iomap_base = sizeof(tss);

    uint32_t tss_base = (uint32_t)&tss;
    uint32_t tss_limit = sizeof(tss) - 1;
    gdt_set_entry(3, tss_base, tss_limit, 0x89, 0x00);

    /* User code/data would be entries 4,5 — not needed yet */

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt;

    /* Load GDT and reload segments */
    __asm__ __volatile__(
        "lgdt %0\n"
        "ljmp $0x08, $1f\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : "m"(gdtp) : "eax", "memory"
    );

    /* Load TSS */
    __asm__ __volatile__(
        "mov $0x18, %%ax\n"
        "ltr %%ax\n"
        : : : "eax"
    );

    return INIT_OK;
}

void gdt_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}
