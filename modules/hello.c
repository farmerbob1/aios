/* modules/hello.c — Hello world KAOS module */
#include <kaos/module.h>

extern void serial_printf(const char* fmt, ...);

static int hello_init(void) {
    serial_printf("[hello] Hello from KAOS module!\n");
    return 0;
}

static void hello_cleanup(void) {
    serial_printf("[hello] Goodbye from KAOS module!\n");
}

kaos_module_info_t kaos_module_info = {
    .magic       = KAOS_MODULE_MAGIC,
    .abi_version = KAOS_ABI_VERSION,
    .name        = "hello",
    .version     = "1.0",
    .author      = NULL,
    .description = "Hello world test module",
    .init        = hello_init,
    .cleanup     = hello_cleanup,
    .dependencies = NULL,
    .flags       = KAOS_FLAG_AUTOLOAD,
};
