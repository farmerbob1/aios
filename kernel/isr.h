/* AIOS v2 — CPU Exception Handlers (Phase 2) */

#pragma once

#include "../include/types.h"

struct registers {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
};

void isr_init(void);
