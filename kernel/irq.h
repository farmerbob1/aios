/* AIOS v2 — Hardware IRQ Handlers (Phase 2) */

#pragma once

#include "../include/types.h"

void irq_init(void);
void irq_register_handler(int irq, void (*handler)(void));
void irq_mask(int irq);
void irq_unmask(int irq);
