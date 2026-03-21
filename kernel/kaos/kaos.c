/* kernel/kaos/kaos.c — KAOS module manager
 *
 * Registry, lifecycle management, dependency resolution, auto-load. */

#include "kaos.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../chaos/chaos.h"

static struct kaos_module modules[KAOS_MAX_MODULES];
static int module_count = 0;

/* ── Init ─────────────────────────────────────────── */

init_result_t kaos_init(void) {
    memset(modules, 0, sizeof(modules));
    module_count = 0;
    serial_printf("[kaos] Module system initialized (%u exported symbols)\n",
                  kaos_sym_count());
    return INIT_OK;
}

/* ── Internal helpers ─────────────────────────────── */

static int find_free_slot(void) {
    for (int i = 0; i < KAOS_MAX_MODULES; i++) {
        if (modules[i].state == KAOS_STATE_UNUSED ||
            modules[i].state == KAOS_STATE_UNLOADED ||
            modules[i].state == KAOS_STATE_LOAD_FAILED) {
            return i;
        }
    }
    return -1;
}

/* Check if name ends with suffix */
static int str_endswith(const char* str, const char* suffix) {
    uint32_t slen = strlen(str);
    uint32_t xlen = strlen(suffix);
    if (xlen > slen) return 0;
    return strcmp(str + slen - xlen, suffix) == 0;
}

/* ── Load ─────────────────────────────────────────── */

int kaos_load(const char* path) {
    int slot = find_free_slot();
    if (slot < 0) {
        serial_printf("[kaos] module table full\n");
        return -1;
    }

    struct kaos_module* mod = &modules[slot];
    memset(mod, 0, sizeof(*mod));
    mod->state = KAOS_STATE_LOADING;
    strncpy(mod->path, path, sizeof(mod->path) - 1);

    /* Load and relocate ELF */
    int rc = kaos_loader_load(path, mod);
    if (rc != 0) {
        mod->state = KAOS_STATE_LOAD_FAILED;
        serial_printf("[kaos] load failed: %s\n", path);
        return -1;
    }

    /* Copy name from module info */
    if (mod->info->name) {
        strncpy(mod->name, mod->info->name, sizeof(mod->name) - 1);
    }

    /* Handle dependencies */
    if (mod->info->dependencies) {
        const char** deps = mod->info->dependencies;
        for (int d = 0; deps[d] != 0; d++) {
            const char* dep_name = deps[d];
            int dep_idx = kaos_find(dep_name);
            if (dep_idx < 0) {
                /* Try to load dependency from /system/modules/<name>.kaos */
                char dep_path[128];
                strncpy(dep_path, "/system/modules/", sizeof(dep_path) - 1);
                uint32_t plen = strlen(dep_path);
                strncpy(dep_path + plen, dep_name,
                        sizeof(dep_path) - plen - 6);
                plen = strlen(dep_path);
                strncpy(dep_path + plen, ".kaos",
                        sizeof(dep_path) - plen - 1);

                dep_idx = kaos_load(dep_path);
                if (dep_idx < 0) {
                    serial_printf("[kaos] dependency '%s' failed for '%s'\n",
                                  dep_name, mod->name);
                    kaos_loader_unload(mod);
                    mod->state = KAOS_STATE_LOAD_FAILED;
                    return -1;
                }
            }
        }
    }

    /* Call init */
    if (mod->info->init) {
        int init_rc = mod->info->init();
        if (init_rc != 0) {
            serial_printf("[kaos] init() failed for '%s' (rc=%d)\n",
                          mod->name, init_rc);
            kaos_loader_unload(mod);
            mod->state = KAOS_STATE_LOAD_FAILED;
            return -1;
        }
    }

    mod->state = KAOS_STATE_LOADED;
    module_count++;
    serial_printf("[kaos] loaded: %s v%s (%u pages @ 0x%08x)\n",
                  mod->name,
                  mod->info->version ? mod->info->version : "?",
                  mod->load_pages, mod->load_base);
    return slot;
}

/* ── Unload ───────────────────────────────────────── */

int kaos_unload(int index) {
    if (index < 0 || index >= KAOS_MAX_MODULES) return -1;

    struct kaos_module* mod = &modules[index];
    if (mod->state != KAOS_STATE_LOADED) {
        serial_printf("[kaos] module %d not loaded (state=%d)\n",
                      index, mod->state);
        return -1;
    }

    /* Check essential flag */
    if (mod->info && (mod->info->flags & KAOS_FLAG_ESSENTIAL)) {
        serial_printf("[kaos] cannot unload essential module '%s'\n",
                      mod->name);
        return -1;
    }

    /* Check if any other loaded module depends on this one */
    for (int i = 0; i < KAOS_MAX_MODULES; i++) {
        if (i == index) continue;
        if (modules[i].state != KAOS_STATE_LOADED) continue;
        if (!modules[i].info || !modules[i].info->dependencies) continue;

        const char** deps = modules[i].info->dependencies;
        for (int d = 0; deps[d] != 0; d++) {
            if (strcmp(deps[d], mod->name) == 0) {
                serial_printf("[kaos] cannot unload '%s': '%s' depends on it\n",
                              mod->name, modules[i].name);
                return -1;
            }
        }
    }

    mod->state = KAOS_STATE_UNLOADING;

    /* Call cleanup */
    if (mod->info && mod->info->cleanup) {
        mod->info->cleanup();
    }

    /* Free memory */
    kaos_loader_unload(mod);
    mod->state = KAOS_STATE_UNLOADED;
    module_count--;

    serial_printf("[kaos] unloaded: %s\n", mod->name);
    return 0;
}

/* ── Auto-load ────────────────────────────────────── */

extern void boot_splash_module(const char *name, int loaded, int total);

void kaos_load_all(const char* directory) {
    int dh = chaos_opendir(directory);
    if (dh < 0) {
        serial_printf("[kaos] cannot open directory: %s\n", directory);
        return;
    }

    /* First pass: count .kaos files (skip synthetic test modules) */
    struct chaos_dirent de;
    char full_path[128];
    int total = 0;

    while (chaos_readdir(dh, &de) == 0) {
        if (de.inode == 0) continue;
        if (!str_endswith(de.name, ".kaos")) continue;
        /* Skip known bad test modules */
        if (strcmp(de.name, "corrupt.kaos") == 0) continue;
        if (strcmp(de.name, "bad_abi.kaos") == 0) continue;
        total++;
    }
    chaos_closedir(dh);

    /* Second pass: load modules with boot splash callback */
    dh = chaos_opendir(directory);
    if (dh < 0) return;

    int loaded = 0;
    while (chaos_readdir(dh, &de) == 0) {
        if (de.inode == 0) continue;
        if (!str_endswith(de.name, ".kaos")) continue;
        if (strcmp(de.name, "corrupt.kaos") == 0) continue;
        if (strcmp(de.name, "bad_abi.kaos") == 0) continue;

        /* Build full path */
        strncpy(full_path, directory, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
        uint32_t dlen = strlen(full_path);
        /* Ensure trailing slash */
        if (dlen > 0 && full_path[dlen - 1] != '/') {
            if (dlen < sizeof(full_path) - 1) {
                full_path[dlen] = '/';
                full_path[dlen + 1] = '\0';
                dlen++;
            }
        }
        strncpy(full_path + dlen, de.name, sizeof(full_path) - dlen - 1);

        int rc = kaos_load(full_path);
        loaded++;

        /* Extract module name from filename for splash (strip .kaos) */
        char mod_name[64];
        strncpy(mod_name, de.name, sizeof(mod_name) - 1);
        mod_name[sizeof(mod_name) - 1] = '\0';
        uint32_t nlen = strlen(mod_name);
        if (nlen > 5 && strcmp(mod_name + nlen - 5, ".kaos") == 0) {
            mod_name[nlen - 5] = '\0';
        }
        boot_splash_module(mod_name, loaded, total);
        (void)rc;
    }

    chaos_closedir(dh);
}

/* ── Query ────────────────────────────────────────── */

int kaos_find(const char* name) {
    for (int i = 0; i < KAOS_MAX_MODULES; i++) {
        if (modules[i].state == KAOS_STATE_LOADED &&
            strcmp(modules[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const struct kaos_module* kaos_get(int index) {
    if (index < 0 || index >= KAOS_MAX_MODULES) return 0;
    if (modules[index].state == KAOS_STATE_UNUSED) return 0;
    return &modules[index];
}

int kaos_get_count(void) {
    int count = 0;
    for (int i = 0; i < KAOS_MAX_MODULES; i++) {
        if (modules[i].state == KAOS_STATE_LOADED) count++;
    }
    return count;
}
