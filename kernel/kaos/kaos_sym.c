/* kernel/kaos/kaos_sym.c — Runtime kernel symbol table
 *
 * The .kaos_export linker section contains kaos_export_entry structs
 * placed by KAOS_EXPORT() macros throughout the kernel. This file
 * provides runtime lookup against that table. */

#include "../../include/kaos/export.h"
#include "../../include/string.h"

extern struct kaos_export_entry __kaos_export_start;
extern struct kaos_export_entry __kaos_export_end;

uint32_t kaos_sym_lookup(const char* name) {
    struct kaos_export_entry* e = &__kaos_export_start;
    struct kaos_export_entry* end = &__kaos_export_end;
    for (; e < end; e++) {
        if (strcmp(e->name, name) == 0) return e->addr;
    }
    return 0;
}

uint32_t kaos_sym_count(void) {
    return ((uint32_t)&__kaos_export_end - (uint32_t)&__kaos_export_start)
           / sizeof(struct kaos_export_entry);
}
