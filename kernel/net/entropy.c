/* AIOS v2 — Entropy Pool for TLS (Phase 11e)
 *
 * Gathers entropy from multiple sources:
 * - RDTSC low bits (microarchitectural jitter at each timer tick)
 * - IRQ timing (intervals between interrupts vary)
 * - Input events (keyboard/mouse are unpredictable)
 *
 * Uses XOR + rotate mixing into a 32-byte pool.
 * After 256 mix events (~1 second at 250Hz), pool is considered ready. */

#include "entropy.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"

static uint8_t pool[ENTROPY_POOL_SIZE];
static uint32_t mix_count = 0;
static uint32_t pool_idx = 0;

/* Read RDTSC (64-bit timestamp counter) */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void entropy_init(void) {
    memset(pool, 0, ENTROPY_POOL_SIZE);
    mix_count = 0;
    pool_idx = 0;

    /* Seed with initial RDTSC value */
    uint64_t tsc = rdtsc();
    for (int i = 0; i < 8; i++) {
        pool[i] = (uint8_t)(tsc >> (i * 8));
    }
}

/* Mix a byte into the pool with XOR and rotation */
static void mix_byte(uint8_t b) {
    pool[pool_idx] ^= b;
    /* Rotate the XOR'd byte */
    pool[pool_idx] = (pool[pool_idx] << 3) | (pool[pool_idx] >> 5);
    /* Cascade: affect next byte too */
    uint32_t next = (pool_idx + 1) % ENTROPY_POOL_SIZE;
    pool[next] ^= pool[pool_idx];
    pool_idx = next;
    mix_count++;
}

void entropy_add_rdtsc(void) {
    uint64_t tsc = rdtsc();
    /* Low bits have the most jitter */
    mix_byte((uint8_t)(tsc));
    mix_byte((uint8_t)(tsc >> 8));
}

void entropy_add_irq(uint8_t irq_num) {
    uint64_t tsc = rdtsc();
    mix_byte((uint8_t)(tsc) ^ irq_num);
}

void entropy_add_byte(uint8_t b) {
    mix_byte(b);
}

bool entropy_ready(void) {
    return mix_count >= 256;
}

/* Generate output bytes from the pool using a simple PRNG seeded by pool state */
int entropy_get(uint8_t *out, size_t len) {
    if (!entropy_ready()) return -1;

    /* Use pool as seed for output generation.
     * XOR pool bytes with RDTSC for each output byte. */
    for (size_t i = 0; i < len; i++) {
        uint64_t tsc = rdtsc();
        uint8_t idx = (uint8_t)((i + tsc) % ENTROPY_POOL_SIZE);
        out[i] = pool[idx] ^ (uint8_t)(tsc >> 4) ^ (uint8_t)(i * 0x9E);
        /* Re-mix to prevent repeated output */
        mix_byte((uint8_t)(tsc));
    }
    return 0;
}
