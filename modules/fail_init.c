/* modules/fail_init.c — Module whose init() deliberately fails */
#include <kaos/module.h>

extern void serial_printf(const char* fmt, ...);

static int fail_init(void) {
    serial_printf("[fail_init] Deliberately failing init\n");
    return -1;
}

static void fail_cleanup(void) {
    /* This should NEVER be called — init failed */
    serial_printf("[fail_init] ERROR: cleanup called after failed init!\n");
}

KAOS_MODULE("fail_init", "1.0", fail_init, fail_cleanup);
