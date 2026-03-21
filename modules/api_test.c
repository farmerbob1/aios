/* modules/api_test.c — Tests kernel API calls from a module */
#include <kaos/module.h>

extern void serial_printf(const char* fmt, ...);
extern void* kmalloc(unsigned int size);
extern void  kfree(void* ptr);

static int api_test_init(void) {
    serial_printf("[api_test] Testing kernel API from module\n");

    /* Test kmalloc/kfree */
    void* p = kmalloc(64);
    if (p) {
        serial_printf("[api_test] kmalloc OK: %p\n", (unsigned int)p);
        kfree(p);
        serial_printf("[api_test] kfree OK\n");
    } else {
        serial_printf("[api_test] kmalloc FAILED\n");
        return -1;
    }

    return 0;
}

KAOS_MODULE("api_test", "1.0", api_test_init, NULL);
