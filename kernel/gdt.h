/* AIOS v2 — Global Descriptor Table (Phase 2) */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

init_result_t gdt_init(void);
void gdt_set_kernel_stack(uint32_t esp0);
