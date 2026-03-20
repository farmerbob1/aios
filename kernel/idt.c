/* AIOS v2 — Interrupt Descriptor Table (Phase 2) */

#include "idt.h"
#include "../include/string.h"

struct idt_entry {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  type;
    uint16_t base_hi;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

void idt_set_gate(uint8_t num, uint32_t handler, uint16_t sel, uint8_t type) {
    idt[num].base_lo = handler & 0xFFFF;
    idt[num].base_hi = (handler >> 16) & 0xFFFF;
    idt[num].sel     = sel;
    idt[num].zero    = 0;
    idt[num].type    = type;
}

init_result_t idt_init(void) {
    memset(idt, 0, sizeof(idt));

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    __asm__ __volatile__("lidt %0" : : "m"(idtp));

    return INIT_OK;
}
