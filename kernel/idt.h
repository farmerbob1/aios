/* AIOS v2 — Interrupt Descriptor Table (Phase 2) */

#pragma once

#include "../include/types.h"
#include "../include/boot_info.h"

init_result_t idt_init(void);
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t sel, uint8_t type);
