/* kernel/kaos/kaos.h — KAOS (Kernel Add-On System) public API */

#ifndef KAOS_H
#define KAOS_H

#include "../../include/boot_info.h"
#include "kaos_types.h"

/* Initialize the module manager. Call after ChaosFS mount. */
init_result_t kaos_init(void);

/* Scan directory and auto-load all .kaos files */
void kaos_load_all(const char* directory);

/* Load a specific module by ChaosFS path. Returns module index or -1. */
int kaos_load(const char* path);

/* Unload a loaded module by index. Returns 0 on success, -1 on error. */
int kaos_unload(int index);

/* Find a loaded module by name. Returns index or -1. */
int kaos_find(const char* name);

/* Get module info by index. NULL if invalid. */
const struct kaos_module* kaos_get(int index);

/* Get count of modules in non-UNUSED state */
int kaos_get_count(void);

/* Symbol table (from kaos_sym.c) */
uint32_t kaos_sym_lookup(const char* name);
uint32_t kaos_sym_count(void);

/* Loader (from kaos_loader.c) */
int kaos_loader_load(const char* path, struct kaos_module* mod);
void kaos_loader_unload(struct kaos_module* mod);

#endif /* KAOS_H */
