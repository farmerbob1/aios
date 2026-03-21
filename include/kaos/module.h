/* include/kaos/module.h — KAOS module author header
 *
 * Every .kaos module includes this to define its kaos_module_info struct. */

#ifndef KAOS_MODULE_H
#define KAOS_MODULE_H

#include "../types.h"

#define KAOS_MODULE_MAGIC   0x4B414F53  /* 'KAOS' */
#define KAOS_ABI_VERSION    1

/* Module flags */
#define KAOS_FLAG_ESSENTIAL  (1 << 0)  /* Cannot be unloaded */
#define KAOS_FLAG_AUTOLOAD   (1 << 1)  /* Load at boot from /modules/ */

typedef struct {
    uint32_t    magic;
    uint32_t    abi_version;
    const char* name;
    const char* version;
    const char* author;
    const char* description;

    int  (*init)(void);
    void (*cleanup)(void);

    const char** dependencies;  /* NULL-terminated array, or NULL */
    uint32_t flags;
} kaos_module_info_t;

/* Convenience macro for module authors */
#define KAOS_MODULE(mod_name, mod_version, mod_init, mod_cleanup) \
    kaos_module_info_t kaos_module_info = { \
        .magic       = KAOS_MODULE_MAGIC, \
        .abi_version = KAOS_ABI_VERSION, \
        .name        = mod_name, \
        .version     = mod_version, \
        .author      = NULL, \
        .description = NULL, \
        .init        = mod_init, \
        .cleanup     = mod_cleanup, \
        .dependencies = NULL, \
        .flags       = 0, \
    }

#endif /* KAOS_MODULE_H */
