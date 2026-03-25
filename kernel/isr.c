/* AIOS v2 — CPU Exception Handlers (Phase 2) */

#include "isr.h"
#include "idt.h"
#include "panic.h"
#include "../drivers/serial.h"

/* Extern ISR stubs from isr.asm */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

static const char* exception_names[32] = {
    "Division by zero",       "Debug",
    "NMI",                    "Breakpoint",
    "Overflow",               "Bound range exceeded",
    "Invalid opcode",         "Device not available",
    "Double fault",           "Coprocessor segment overrun",
    "Invalid TSS",            "Segment not present",
    "Stack-segment fault",    "General protection fault",
    "Page fault",             "Reserved",
    "x87 FP exception",      "Alignment check",
    "Machine check",          "SIMD FP exception",
    "Virtualization",         "Control protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved",
    "VMM communication",      "Security exception",
    "Reserved"
};

void isr_init(void) {
    typedef void (*isr_fn)(void);
    isr_fn stubs[32] = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };

    for (int i = 0; i < 32; i++) {
        idt_set_gate((uint8_t)i, (uint32_t)stubs[i], 0x08, 0x8E);
    }
}

void isr_common_handler(struct registers* regs) {
    serial_printf("\n!!! CPU Exception %u: %s\n", regs->int_no,
                  regs->int_no < 32 ? exception_names[regs->int_no] : "Unknown");
    serial_printf("  Error code: 0x%08x\n", regs->err_code);
    serial_printf("  EIP: 0x%08x  CS: 0x%04x  EFLAGS: 0x%08x\n",
                  regs->eip, regs->cs, regs->eflags);
    serial_printf("  EAX: 0x%08x  EBX: 0x%08x  ECX: 0x%08x  EDX: 0x%08x\n",
                  regs->eax, regs->ebx, regs->ecx, regs->edx);
    serial_printf("  ESI: 0x%08x  EDI: 0x%08x  EBP: 0x%08x  ESP: 0x%08x\n",
                  regs->esi, regs->edi, regs->ebp, regs->esp_dummy);

    if (regs->int_no == 14) {
        uint32_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        serial_printf("  CR2 (fault addr): 0x%08x\n", cr2);

        /* Diagnose page table state for the faulting address */
        uint32_t cr3_val;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));
        uint32_t* pd = (uint32_t*)(cr3_val & 0xFFFFF000);
        uint32_t pd_idx = cr2 >> 22;
        uint32_t pt_idx = (cr2 >> 12) & 0x3FF;
        uint32_t pde = pd[pd_idx];
        serial_printf("  PDE[%u]=0x%08x", pd_idx, pde);
        if (pde & 1) {
            uint32_t* pt = (uint32_t*)(pde & 0xFFFFF000);
            serial_printf(" PTE[%u]=0x%08x", pt_idx, pt[pt_idx]);
        }
        serial_printf("\n");
    }

    kernel_panic("Unhandled CPU exception");
}
