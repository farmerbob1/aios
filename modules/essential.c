/* modules/essential.c — Module marked as essential (cannot unload) */
#include <kaos/module.h>

extern void serial_printf(const char* fmt, ...);

static int essential_init(void) {
    serial_printf("[essential] Essential module loaded\n");
    return 0;
}

static void essential_cleanup(void) {
    serial_printf("[essential] Essential module cleanup\n");
}

kaos_module_info_t kaos_module_info = {
    .magic       = KAOS_MODULE_MAGIC,
    .abi_version = KAOS_ABI_VERSION,
    .name        = "essential",
    .version     = "1.0",
    .author      = NULL,
    .description = "Essential test module",
    .init        = essential_init,
    .cleanup     = essential_cleanup,
    .dependencies = NULL,
    .flags       = KAOS_FLAG_ESSENTIAL,
};
