/* AIOS v2 — High-Resolution Timestamp Counter (Phase 3) */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

init_result_t rdtsc_calibrate(void);
uint64_t rdtsc_get_frequency(void);
uint64_t rdtsc_to_us(uint64_t cycles);
