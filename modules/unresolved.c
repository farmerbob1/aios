/* modules/unresolved.c — Module that references a nonexistent symbol */
#include <kaos/module.h>

extern void serial_printf(const char* fmt, ...);
extern int nonexistent_function(void);

static int unresolved_init(void) {
    serial_printf("[unresolved] This should never print\n");
    nonexistent_function();
    return 0;
}

KAOS_MODULE("unresolved", "1.0", unresolved_init, NULL);
