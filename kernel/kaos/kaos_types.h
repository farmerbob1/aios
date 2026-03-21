/* kernel/kaos/kaos_types.h — ELF32 structures, KAOS types and state machine */

#ifndef KAOS_TYPES_H
#define KAOS_TYPES_H

#include "../../include/types.h"
#include "../../include/kaos/module.h"

/* ── ELF32 Definitions ──────────────────────────────── */

#define EI_NIDENT    16
#define ET_REL       1
#define EM_386       3
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_REL      9
#define SHT_NOBITS   8
#define SHF_ALLOC    0x2
#define SHN_UNDEF    0
#define STB_GLOBAL   1
#define R_386_32     1
#define R_386_PC32   2

#define ELF32_R_SYM(i)   ((i) >> 8)
#define ELF32_R_TYPE(i)   ((i) & 0xFF)
#define ELF32_ST_BIND(i)  ((i) >> 4)
#define ELF32_ST_TYPE(i)  ((i) & 0xF)

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
} __attribute__((packed)) Elf32_Shdr;

typedef struct {
    uint32_t st_name, st_value, st_size;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
} __attribute__((packed)) Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} __attribute__((packed)) Elf32_Rel;

/* ── KAOS Module State Machine ───────────────────────── */

typedef enum {
    KAOS_STATE_UNUSED = 0,
    KAOS_STATE_DISCOVERED,
    KAOS_STATE_LOADING,
    KAOS_STATE_LOADED,
    KAOS_STATE_LOAD_FAILED,
    KAOS_STATE_UNLOADING,
    KAOS_STATE_UNLOADED,
} kaos_state_t;

/* ── Module Registry Entry ───────────────────────────── */

#define KAOS_MAX_MODULES  32

struct kaos_module {
    kaos_state_t         state;
    char                 name[64];
    char                 path[128];

    /* Memory */
    uint32_t             load_base;   /* physical address of allocated memory */
    uint32_t             load_pages;  /* number of PMM pages allocated */

    /* Module info (valid only in LOADED state) */
    kaos_module_info_t*  info;        /* points into loaded module memory */
};

#endif /* KAOS_TYPES_H */
