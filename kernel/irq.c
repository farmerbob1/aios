/* AIOS v2 — Hardware IRQ Handlers (Phase 2) */

#include "irq.h"
#include "idt.h"
#include "isr.h"
#include "../include/io.h"
#include "../include/kaos/export.h"
#include "net/entropy.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

/* Extern IRQ stubs from irq.asm */
extern void irq0(void);  extern void irq1(void);
extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);
extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void);
extern void irq14(void); extern void irq15(void);

#define IRQ_MAX_SHARED 4
static void (*irq_handlers[16][IRQ_MAX_SHARED])(void);
static int irq_handler_count[16];

static void pic_remap(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: begin initialization */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, 0x20); io_wait();  /* IRQ 0-7  → INT 32-39 */
    outb(PIC2_DATA, 0x28); io_wait();  /* IRQ 8-15 → INT 40-47 */

    /* ICW3: cascading */
    outb(PIC1_DATA, 0x04); io_wait();  /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* cascade identity */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Restore saved masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void irq_init(void) {
    pic_remap();

    /* Mask all IRQs initially */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    /* Wire IRQ stubs into IDT */
    typedef void (*irq_fn)(void);
    irq_fn stubs[16] = {
        irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
        irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15
    };

    for (int i = 0; i < 16; i++) {
        idt_set_gate((uint8_t)(32 + i), (uint32_t)stubs[i], 0x08, 0x8E);
        for (int j = 0; j < IRQ_MAX_SHARED; j++)
            irq_handlers[i][j] = NULL;
        irq_handler_count[i] = 0;
    }
}

void irq_register_handler(int irq, void (*handler)(void)) {
    if (irq >= 0 && irq < 16 && handler) {
        int n = irq_handler_count[irq];
        if (n < IRQ_MAX_SHARED) {
            irq_handlers[irq][n] = handler;
            irq_handler_count[irq] = n + 1;
        }
    }
}

void irq_mask(int irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t val = inb(port) | (1 << irq);
    outb(port, val);
}

void irq_unmask(int irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
        /* Unmask cascade (IRQ2) on master when unmasking slave IRQs */
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 2));
    }
    uint8_t val = inb(port) & ~(1 << irq);
    outb(port, val);
}

/* CRITICAL: Send EOI BEFORE calling handler.
 * If handler calls schedule()→task_switch(), EOI would be deferred until
 * the old task resumes, stopping the timer. Sending EOI first is safe
 * because IF=0 (interrupt gate), so no re-entrance. */
void irq_common_handler(struct registers* regs) {
    int irq_num = (int)(regs->int_no - 32);

    /* Send EOI first */
    if (irq_num >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);

    /* Collect entropy from IRQ timing */
    entropy_add_irq((uint8_t)irq_num);

    /* Then call all registered handlers for this IRQ (shared IRQ support) */
    if (irq_num >= 0 && irq_num < 16) {
        for (int i = 0; i < irq_handler_count[irq_num]; i++) {
            if (irq_handlers[irq_num][i])
                irq_handlers[irq_num][i]();
        }
    }
}

KAOS_EXPORT(irq_register_handler)
KAOS_EXPORT(irq_mask)
KAOS_EXPORT(irq_unmask)
