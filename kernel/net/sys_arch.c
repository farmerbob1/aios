/* AIOS v2 — lwIP sys_arch implementation (NO_SYS=1)
 *
 * Minimal porting layer for lwIP in NO_SYS mode:
 * - sys_now() returns milliseconds since boot
 * - sys_arch_protect/unprotect for interrupt-safe critical sections
 * - sys_init() does nothing */

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "../../drivers/serial.h"

/* Timer API */
extern uint32_t timer_get_ticks(void);
extern uint32_t timer_get_frequency(void);

/* Random seed (used by LWIP_RAND in cc.h) */
unsigned int lwip_rand_seed = 0x12345678;

/* ── sys_now: milliseconds since boot ───────────────────── */

u32_t sys_now(void) {
    return (u32_t)(timer_get_ticks() * 1000 / timer_get_frequency());
}

/* ── sys_init: called by lwip_init() ────────────────────── */

void sys_init(void) {
    /* Nothing to do for NO_SYS=1 */
}

/* ── SYS_LIGHTWEIGHT_PROT: interrupt-based protection ───── */

sys_prot_t sys_arch_protect(void) {
    sys_prot_t flags;
    __asm__ __volatile__("pushfl; cli; popl %0" : "=r"(flags));
    return flags;
}

void sys_arch_unprotect(sys_prot_t val) {
    __asm__ __volatile__("pushl %0; popfl" : : "r"(val));
}
