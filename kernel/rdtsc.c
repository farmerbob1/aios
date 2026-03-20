/* AIOS v2 — RDTSC Calibration (Phase 2) */

#include "rdtsc.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"

static uint64_t tsc_frequency;

init_result_t rdtsc_calibrate(void) {
    /* Wait for a tick boundary */
    uint64_t start_ticks = timer_get_ticks();
    while (timer_get_ticks() == start_ticks) {
        __asm__ __volatile__("hlt");
    }

    /* Measure TSC over 100 ticks (400ms at 250Hz) */
    uint64_t tsc_start = rdtsc();
    uint64_t tick_start = timer_get_ticks();

    uint64_t target = tick_start + 100;
    while (timer_get_ticks() < target) {
        __asm__ __volatile__("hlt");
    }

    uint64_t tsc_end = rdtsc();
    uint64_t elapsed_ticks = timer_get_ticks() - tick_start;

    /* cycles per second = (tsc_delta / elapsed_ticks) * PIT_FREQUENCY */
    uint64_t tsc_delta = tsc_end - tsc_start;
    tsc_frequency = (tsc_delta * PIT_FREQUENCY) / elapsed_ticks;

    serial_printf("  RDTSC: %u MHz\n", (uint32_t)(tsc_frequency / 1000000));

    return INIT_OK;
}

uint64_t rdtsc_get_frequency(void) {
    return tsc_frequency;
}

uint64_t rdtsc_to_us(uint64_t cycles) {
    if (tsc_frequency == 0) return 0;
    return (cycles * 1000000) / tsc_frequency;
}
