/* modules/dep_b.c — Base dependency module (dep_a depends on this) */
#include <kaos/module.h>

extern void serial_printf(const char* fmt, ...);

static int dep_b_init(void) {
    serial_printf("[dep_b] Dependency base module loaded\n");
    return 0;
}

static void dep_b_cleanup(void) {
    serial_printf("[dep_b] Dependency base module cleanup\n");
}

KAOS_MODULE("dep_b", "1.0", dep_b_init, dep_b_cleanup);
