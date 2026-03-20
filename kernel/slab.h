/* AIOS v2 — Slab Allocator (Phase 1)
 * O(1) alloc/free for fixed-size blocks <= 2048 bytes. */

#pragma once

#include "../include/types.h"

void     slab_init(void);
void*    slab_alloc(size_t size);
void     slab_free(void* ptr);
uint32_t slab_get_total_alloc(void);
