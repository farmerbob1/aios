/* AIOS v2 — PIT Timer Driver (Phase 2) */

#include "timer.h"
#include "../include/io.h"
#include "../kernel/irq.h"
#include "../kernel/scheduler.h"

#define PIT_CH0_DATA 0x40
#define PIT_CMD      0x43

static volatile uint64_t ticks;

static uint64_t timer_read_ticks(void) {
    uint64_t val;
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags));
    val = ticks;
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags));
    return val;
}

void timer_handler(void) {
    ticks++;
    schedule();
}

init_result_t timer_init(void) {
    ticks = 0;

    /* PIT channel 0, mode 2 (rate generator), lo/hi byte, binary */
    uint16_t divisor = PIT_BASE_FREQ / PIT_FREQUENCY;  /* 4773 */
    outb(PIT_CMD, 0x34);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    irq_register_handler(0, timer_handler);
    irq_unmask(0);

    return INIT_OK;
}

uint64_t timer_get_ticks(void) {
    return timer_read_ticks();
}

uint32_t timer_get_uptime_seconds(void) {
    return (uint32_t)(timer_read_ticks() / PIT_FREQUENCY);
}

uint32_t timer_get_frequency(void) {
    return PIT_FREQUENCY;
}

void timer_wait(uint32_t ms) {
    uint64_t target = timer_read_ticks() + ((uint64_t)ms * PIT_FREQUENCY) / 1000;
    while (timer_read_ticks() < target) {
        __asm__ __volatile__("hlt");
    }
}
