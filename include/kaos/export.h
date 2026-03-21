/* include/kaos/export.h — KAOS kernel symbol export macro
 *
 * Used by kernel source files to mark functions for runtime
 * symbol resolution by loaded modules. */

#ifndef KAOS_EXPORT_H
#define KAOS_EXPORT_H

#include "../types.h"

struct kaos_export_entry {
    const char* name;   /* symbol name string */
    uint32_t    addr;   /* symbol address */
};

/* Place exported symbol in .kaos_export ELF section.
 * The linker script collects these; kaos_sym_lookup() scans them at runtime. */
#define KAOS_EXPORT(sym) \
    __attribute__((used, section(".kaos_export"))) \
    static const struct kaos_export_entry __kaos_export_##sym = { \
        .name = #sym, \
        .addr = (uint32_t)&sym \
    };

#endif /* KAOS_EXPORT_H */
