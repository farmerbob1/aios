/* AIOS v2 — FPU/SSE Initialization (Phase 2) */

#include "fpu.h"
#include "../include/types.h"

void fpu_init(void) {
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2);  /* Clear CR0.EM (bit 2) — no FPU emulation */
    cr0 |=  (1 << 1);  /* Set CR0.MP (bit 1) — monitor coprocessor */
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));

    uint32_t cr4;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);   /* CR4.OSFXSR — enable FXSAVE/FXRSTOR */
    cr4 |= (1 << 10);  /* CR4.OSXMMEXCPT — enable SIMD exceptions */
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));

    __asm__ __volatile__("fninit");
}
