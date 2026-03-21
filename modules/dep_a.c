/* modules/dep_a.c — Module that depends on dep_b */
#include <kaos/module.h>

extern void serial_printf(const char* fmt, ...);

static const char* my_deps[] = { "dep_b", (const char*)0 };

static int dep_a_init(void) {
    serial_printf("[dep_a] Module with dependency loaded\n");
    return 0;
}

static void dep_a_cleanup(void) {
    serial_printf("[dep_a] Module with dependency cleanup\n");
}

kaos_module_info_t kaos_module_info = {
    .magic       = KAOS_MODULE_MAGIC,
    .abi_version = KAOS_ABI_VERSION,
    .name        = "dep_a",
    .version     = "1.0",
    .author      = NULL,
    .description = "Module with dependency on dep_b",
    .init        = dep_a_init,
    .cleanup     = dep_a_cleanup,
    .dependencies = my_deps,
    .flags       = 0,
};
