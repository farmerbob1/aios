/* AIOS v2 — Entropy Pool for TLS (Phase 11e)
 *
 * Collects entropy from RDTSC jitter and IRQ timing.
 * Used to seed BearSSL's HMAC_DRBG PRNG. */

#pragma once

#include "../../include/types.h"

#define ENTROPY_POOL_SIZE 32

void entropy_init(void);
void entropy_add_rdtsc(void);           /* Called from timer handler */
void entropy_add_irq(uint8_t irq_num);  /* Called from IRQ handler */
void entropy_add_byte(uint8_t b);       /* Extra entropy source */
int  entropy_get(uint8_t *out, size_t len);  /* Fill buffer with random bytes */
bool entropy_ready(void);               /* True after sufficient mixing */
