# KAOS — Kernel Add-On System Specification
## AIOS v2 Runtime Module System

---

## Overview

KAOS (Kernel Add-On System) provides runtime kernel extensibility for AIOS. Modules are `.kaos` files (ELF relocatable objects, ET_REL) stored on ChaosFS under `/modules/`. The kernel loads them at boot or on demand, processes relocations against a kernel symbol table, and calls their init function.

**Use cases:**
- New hardware drivers (USB, NIC, audio)
- New Lua bindings (modules call `lua_register()` to add functions)
- New Claude tools (modules call `claude_tool_register()`)
- New filesystem drivers (mount FAT, read ISO)
- Self-contained applications packaged as modules

**Design rules:**
1. Modules are compiled as ET_REL (`-c` flag, no linking) with the same `i686-elf-gcc` cross-compiler as the kernel.
2. Every module exports a `kaos_module_info_t` struct and a `kaos_init()` function.
3. The kernel symbol table is the foundation — modules resolve undefined symbols against it at load time.
4. Module memory is allocated from PMM and marked PAGE_RESERVED.
5. ABI version mismatches are rejected at load time, not at runtime.

---

## Kernel Symbol Table

### The Problem

Modules need to call kernel functions (`kmalloc`, `serial_printf`, `irq_register_handler`, etc.). The kernel is a flat binary with no dynamic symbol table — all symbols are resolved at link time and discarded. We need a runtime-queryable symbol table.

### Design: KAOS_EXPORT Macro + Build-Time Generation

**Step 1: Mark exported symbols in kernel source.**

```c
/* include/kaos/export.h */

/* Place exported symbol in a special ELF section.
 * The build system extracts these to generate the symbol table. */
#define KAOS_EXPORT(sym) \
    __attribute__((used, section(".kaos_export"))) \
    static const struct kaos_export_entry __kaos_export_##sym = { \
        .name = #sym, \
        .addr = (uint32_t)&sym \
    };

struct kaos_export_entry {
    const char* name;
    uint32_t    addr;
} __attribute__((packed));
```

**Step 2: Add exports to kernel source files.**

```c
/* In kernel/heap.c */
#include <kaos/export.h>

KAOS_EXPORT(kmalloc)
KAOS_EXPORT(kfree)
KAOS_EXPORT(kzmalloc)
KAOS_EXPORT(kmalloc_aligned)
KAOS_EXPORT(kfree_aligned)
KAOS_EXPORT(krealloc)
```

```c
/* In drivers/serial.c */
KAOS_EXPORT(serial_print)
KAOS_EXPORT(serial_printf)
KAOS_EXPORT(serial_putchar)
```

**Step 3: Linker script collects exports.**

```ld
/* Addition to linker.ld */
.kaos_export : {
    __kaos_export_start = .;
    KEEP(*(.kaos_export))
    __kaos_export_end = .;
}
```

**Step 4: Runtime symbol table.**

```c
/* kernel/kaos/kaos_sym.c */

extern struct kaos_export_entry __kaos_export_start;
extern struct kaos_export_entry __kaos_export_end;

/* Lookup a symbol by name. Returns address or 0 if not found. */
uint32_t kaos_sym_lookup(const char* name) {
    struct kaos_export_entry* e = &__kaos_export_start;
    struct kaos_export_entry* end = &__kaos_export_end;
    for (; e < end; e++) {
        if (strcmp(e->name, name) == 0) return e->addr;
    }
    return 0;
}

/* Get total exported symbol count (for diagnostics). */
uint32_t kaos_sym_count(void) {
    return ((uint32_t)&__kaos_export_end - (uint32_t)&__kaos_export_start)
           / sizeof(struct kaos_export_entry);
}
```

**Why not a linker map script?** The linker map is a host-side text file — parsing it at runtime is fragile and adds a text parser dependency. The `.kaos_export` section approach embeds the table directly in the kernel binary with zero runtime parsing overhead. The table is read-only and lives in `.rodata`-adjacent memory.

### Exported Kernel API

The following symbols MUST be exported via KAOS_EXPORT for modules to be useful:

| Category | Symbols |
|----------|---------|
| Heap | `kmalloc`, `kfree`, `kzmalloc`, `kmalloc_aligned`, `kfree_aligned`, `krealloc`, `kmalloc_usable_size` |
| PMM | `pmm_alloc_page`, `pmm_alloc_pages`, `pmm_free_page`, `pmm_free_pages` |
| VMM | `vmm_map_page`, `vmm_map_range`, `vmm_unmap_page` |
| Serial | `serial_print`, `serial_printf`, `serial_putchar` |
| String | `memcpy`, `memset`, `memcmp`, `memmove`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy` |
| I/O | `inb`, `outb`, `inw`, `outw`, `io_wait` (inline — must be wrapped for export) |
| IRQ | `irq_register_handler`, `irq_mask`, `irq_unmask` |
| Scheduler | `task_create`, `task_sleep`, `task_yield`, `task_exit`, `task_kill`, `task_get_current` |
| Timer | `timer_get_ticks`, `timer_get_frequency`, `timer_wait` |
| ChaosFS | `chaos_open`, `chaos_close`, `chaos_read`, `chaos_write`, `chaos_seek`, `chaos_stat`, `chaos_mkdir`, `chaos_unlink` |
| Panic | `kernel_panic` |

**Note on inline functions:** `inb`, `outb`, etc. are defined as `static inline` in `include/io.h`. Inline functions have no linkable symbol. To export them, create non-inline wrappers in a source file:

```c
/* kernel/kaos/kaos_io_wrappers.c */
#include <io.h>
#include <kaos/export.h>

uint8_t kaos_inb(uint16_t port) { return inb(port); }
void kaos_outb(uint16_t port, uint8_t val) { outb(port, val); }
KAOS_EXPORT(kaos_inb)
KAOS_EXPORT(kaos_outb)
```

Modules use `kaos_inb`/`kaos_outb` instead of the inline versions.

---

## Module Info Struct and ABI Contract

### Module Magic and ABI Version

```c
#define KAOS_MODULE_MAGIC   0x4B414F53  /* 'KAOS' */
#define KAOS_ABI_VERSION    1

/* ABI version is incremented when:
 * - Any exported kernel function signature changes
 * - kaos_module_info_t layout changes
 * - Calling convention or memory model changes
 * ABI version is NOT incremented for:
 * - New exported symbols (additive — old modules still work)
 * - Internal kernel changes that don't affect module API */
```

### Module Info Struct

Every module must define exactly one instance of this struct, named `kaos_module_info`:

```c
typedef struct {
    uint32_t    magic;          /* Must be KAOS_MODULE_MAGIC */
    uint32_t    abi_version;    /* Must match KAOS_ABI_VERSION */
    const char* name;           /* Human-readable module name */
    const char* version;        /* Module version string (e.g., "1.0") */
    const char* author;         /* Module author */
    const char* description;    /* One-line description */

    /* Entry points */
    int  (*init)(void);         /* Called after load + relocation. Return 0 = success. */
    void (*cleanup)(void);      /* Called before unload. NULL if no cleanup needed. */

    /* Dependencies — NULL-terminated array of module names.
     * Loader resolves and loads these BEFORE calling this module's init.
     * NULL = no dependencies. */
    const char** dependencies;

    /* Flags */
    uint32_t flags;
    #define KAOS_FLAG_ESSENTIAL  (1 << 0)  /* Cannot be unloaded (drivers) */
    #define KAOS_FLAG_AUTOLOAD   (1 << 1)  /* Load at boot from /modules/ */
} kaos_module_info_t;

/* Convenience macro for module authors */
#define KAOS_MODULE(mod_name, mod_init, mod_cleanup) \
    kaos_module_info_t kaos_module_info = { \
        .magic       = KAOS_MODULE_MAGIC, \
        .abi_version = KAOS_ABI_VERSION, \
        .name        = mod_name, \
        .init        = mod_init, \
        .cleanup     = mod_cleanup, \
        .dependencies = NULL, \
        .flags       = 0, \
    }
```

---

## ELF Relocatable Loader

### Module File Format

Modules are standard i386 ELF relocatable objects (ET_REL, `e_type == 1`). They are compiled with `-c` (no linking), producing `.o` files renamed to `.kaos`.

The loader handles:
- **Section-based loading** (not segment-based like the bootloader's ET_EXEC loader)
- **Relocation types:** R_386_32 (absolute 32-bit) and R_386_PC32 (PC-relative 32-bit)
- **Symbol resolution** against the kernel symbol table

### Load Sequence

```
1. Read .kaos file from ChaosFS into a temporary heap buffer
2. Validate ELF header:
   - Magic bytes (0x7F 'E' 'L' 'F')
   - 32-bit (EI_CLASS == ELFCLASS32)
   - Little-endian (EI_DATA == ELFDATA2LSB)
   - Relocatable (e_type == ET_REL)
   - i386 (e_machine == EM_386)
3. Parse section headers, identify:
   - .text, .data, .rodata, .bss sections (SHT_PROGBITS / SHT_NOBITS)
   - .symtab (symbol table)
   - .strtab (string table)
   - .rel.text, .rel.data (relocation sections)
4. Allocate contiguous memory from PMM for all loadable sections:
   - Compute total size (sum of sh_size for all SHF_ALLOC sections, page-aligned)
   - pmm_alloc_pages(pages_needed)
   - Mark as PAGE_RESERVED via heap_mark_reserved()
   - Map in VMM with PRESENT | WRITABLE
5. Copy section contents to allocated memory:
   - For each SHF_ALLOC section: memcpy from file buffer to target address
   - For .bss (SHT_NOBITS): memset to zero
   - Record each section's load address (base + offset)
6. Process relocations:
   - For each .rel section, iterate relocation entries
   - For each entry: look up the referenced symbol
     - If symbol is defined in the module (st_shndx != SHN_UNDEF): compute address from section load base + st_value
     - If symbol is undefined (st_shndx == SHN_UNDEF): look up via kaos_sym_lookup()
     - If not found in kernel symbol table: ERROR — unresolved symbol, abort load
   - Apply relocation:
     - R_386_32: *target += symbol_addr
     - R_386_PC32: *target += symbol_addr - target_addr
7. Find kaos_module_info symbol in module's symbol table
   - Validate: magic == KAOS_MODULE_MAGIC, abi_version == KAOS_ABI_VERSION
8. Free the temporary file buffer (no longer needed — sections are copied)
9. Return success — module is ready for init
```

### ELF Structures Used

```c
/* Standard ELF32 definitions needed by the loader */
#define EI_NIDENT   16
#define ET_REL      1
#define EM_386      3
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_REL      9
#define SHT_NOBITS   8
#define SHF_ALLOC    0x2
#define STN_UNDEF    0
#define SHN_UNDEF    0
#define R_386_32     1
#define R_386_PC32   2

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
    uint32_t r_info;  /* ELF32_R_SYM(i) = (i)>>8, ELF32_R_TYPE(i) = (i)&0xff */
} __attribute__((packed)) Elf32_Rel;

#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i)  ((i) & 0xFF)
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xF)
```

---

## Module Lifecycle

### State Machine

```
DISCOVERED → LOADING → LOADED → (optionally) UNLOADING → UNLOADED
                ↓                        ↓
           LOAD_FAILED              UNLOAD_FAILED
```

### States

```c
typedef enum {
    KAOS_STATE_UNUSED = 0,
    KAOS_STATE_DISCOVERED,  /* Found on disk, not yet loaded */
    KAOS_STATE_LOADING,     /* Currently being loaded + relocated */
    KAOS_STATE_LOADED,      /* Fully loaded, init() returned 0 */
    KAOS_STATE_LOAD_FAILED, /* Load or init failed — resources freed */
    KAOS_STATE_UNLOADING,   /* cleanup() called, freeing resources */
    KAOS_STATE_UNLOADED,    /* Fully unloaded — slot can be reused */
} kaos_state_t;
```

### Transitions

**DISCOVERED → LOADING:**
- Module file read from ChaosFS
- ELF validation passes

**LOADING → LOADED:**
- Sections allocated and copied
- Relocations processed successfully
- `kaos_module_info` found and validated
- Dependencies loaded (recursively, if any)
- `init()` called and returned 0

**LOADING → LOAD_FAILED:**
- Any validation failure (bad ELF, ABI mismatch, unresolved symbol)
- `init()` returned non-zero
- All allocated memory freed, slot marked LOAD_FAILED
- Error logged to serial with specific reason

**LOADED → UNLOADING:**
- `modunload` command or `kaos_unload()` API call
- Module does NOT have KAOS_FLAG_ESSENTIAL set
- No other loaded module depends on this one

**UNLOADING → UNLOADED:**
- `cleanup()` called (if non-NULL)
- PMM pages freed, VMM mappings removed
- Module slot available for reuse

---

## Module Manager

### Module Registry

```c
#define KAOS_MAX_MODULES  32

struct kaos_module {
    kaos_state_t state;
    char         name[64];          /* from kaos_module_info.name */
    char         path[128];         /* ChaosFS path (e.g., "/modules/hello.kaos") */

    /* Memory */
    uint32_t     load_base;         /* physical address of allocated memory */
    uint32_t     load_pages;        /* number of PMM pages allocated */

    /* Module info (valid only in LOADED state) */
    kaos_module_info_t* info;       /* points into loaded module memory */
};

static struct kaos_module modules[KAOS_MAX_MODULES];
static int module_count;
```

### Manager API

```c
/* Initialize module manager. Must be called after ChaosFS is mounted. */
init_result_t kaos_init(void);

/* Scan /modules/ and auto-load all .kaos files with KAOS_FLAG_AUTOLOAD. */
void kaos_load_all(const char* directory);

/* Load a specific module by ChaosFS path. Returns module index or -1. */
int kaos_load(const char* path);

/* Unload a loaded module by index. Returns 0 on success, -1 on error.
 * Fails if module is ESSENTIAL or has dependents. */
int kaos_unload(int index);

/* Query */
int kaos_find(const char* name);                   /* Returns index or -1 */
const struct kaos_module* kaos_get(int index);      /* NULL if invalid */
int kaos_get_count(void);
```

### Boot-Time Auto-Load

During `kaos_load_all("/modules/")`:

1. `chaos_opendir("/modules/")`
2. For each `.kaos` file found:
   - Load into DISCOVERED state
   - Attempt full load (ELF parse, relocate, validate info)
   - If `KAOS_FLAG_AUTOLOAD` is set in the module info, call `init()`
   - Log result to serial
3. Dependency ordering: if module A depends on module B, B is loaded first. Circular dependencies are detected and rejected (log error, skip both).

### Shell Commands

```
modload <path>      — Load a module from ChaosFS path
modunload <name>    — Unload a loaded module by name
modlist             — List all modules with state, name, version, memory usage
```

---

## Module SDK

### Headers for Module Authors

```
include/kaos/
├── module.h        # KAOS_MODULE macro, kaos_module_info_t, KAOS_EXPORT
├── kernel.h        # Convenience include for common kernel APIs:
│                   #   types.h, string.h, serial.h, heap.h, scheduler.h
└── export.h        # KAOS_EXPORT macro (shared between kernel and modules)
```

Module authors include `<kaos/module.h>` and optionally `<kaos/kernel.h>`. They do NOT include kernel-internal headers directly.

### Compile Flags for Modules

```makefile
# Module compilation — same target arch, same restrictions as kernel code
MODULE_CFLAGS = -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror \
                -O2 -g -std=c11 -march=core2 -mno-sse -mno-mmx -mno-sse2 \
                -I$(AIOS_ROOT)/include -c

# Build a .kaos module:
#   i686-elf-gcc $(MODULE_CFLAGS) -o mymodule.kaos mymodule.c
```

**Modules use `-mno-sse`** like kernel code. If a module needs SSE (e.g., a renderer plugin), it must save/restore XMM state itself or run in a dedicated task where the scheduler handles FPU state.

### Example: Hello World Module

```c
/* modules/hello.c */
#include <kaos/module.h>
#include <kaos/kernel.h>

static int hello_init(void) {
    serial_printf("[hello] Hello from KAOS module!\n");
    return 0;
}

static void hello_cleanup(void) {
    serial_printf("[hello] Goodbye from KAOS module!\n");
}

KAOS_MODULE("hello", hello_init, hello_cleanup);
```

### Example: Lua Binding Module

```c
/* modules/lua_math.c */
#include <kaos/module.h>
#include <kaos/kernel.h>

/* These would be exported by the Lua subsystem when it exists */
extern int lua_register(const char* name, void* func);

static int l_fast_sqrt(/* lua_State* L */) {
    /* ... */
    return 1;
}

static int lua_math_init(void) {
    lua_register("fast_sqrt", l_fast_sqrt);
    serial_printf("[lua_math] Registered fast_sqrt\n");
    return 0;
}

KAOS_MODULE("lua_math", lua_math_init, NULL);
```

---

## Integration Points

### Driver Module

A driver module registers an IRQ handler and performs I/O:

```c
static void my_irq_handler(void) { /* handle interrupt */ }

static int driver_init(void) {
    irq_register_handler(11, my_irq_handler);
    irq_unmask(11);
    return 0;
}

static void driver_cleanup(void) {
    irq_mask(11);
    irq_register_handler(11, NULL);  /* deregister */
}
```

### Filesystem Module (Future)

A filesystem module registers a new filesystem type with a VFS layer (Phase 7+):

```c
static int fat_init(void) {
    vfs_register_fs("fat32", &fat_ops);
    return 0;
}
```

---

## Memory and Safety Contracts

### Who Owns Module Memory

- **PMM pages** for module code/data are owned by the module manager.
- On `kaos_unload()`, the manager calls `cleanup()` then frees the pages.
- The manager zeroes the memory before freeing (prevent stale code execution).

### Unload Safety

**v1 contract: no reference counting.** Unloading a module that is actively being called (e.g., an IRQ handler registered by the module) causes undefined behavior. The `cleanup()` function is responsible for deregistering all callbacks before returning.

**Essential modules:** modules with `KAOS_FLAG_ESSENTIAL` cannot be unloaded. This is the simple safety mechanism for drivers and core services.

**No hot-swap in v1.** Module code is not relocatable after load. You cannot move a loaded module to a different address.

### What Happens on Init Failure

If `init()` returns non-zero:
1. Log error with module name and return code
2. Free all allocated PMM pages
3. Remove VMM mappings
4. Set state to LOAD_FAILED
5. The module's `cleanup()` is NOT called (init failed, nothing to clean up)

---

## Build System Changes

### Kernel Makefile Additions

```makefile
# New linker script section for .kaos_export
# (Added to linker.ld, between .data and .bss)

# Module build target (example)
modules/%.kaos: modules/%.c
    $(CC) $(MODULE_CFLAGS) -o $@ $<
```

### Linker Script Addition

```ld
/* Add after .data section in linker.ld */
.kaos_export : {
    __kaos_export_start = .;
    KEEP(*(.kaos_export))
    __kaos_export_end = .;
}
```

### New Source Files

```
kernel/kaos/
├── kaos.h              # Public API (kaos_init, kaos_load, kaos_unload, etc.)
├── kaos.c              # Module manager: registry, load_all, shell commands
├── kaos_loader.c       # ELF relocatable loader: parse, relocate, validate
├── kaos_sym.c          # Kernel symbol table: kaos_sym_lookup, kaos_sym_count
├── kaos_io_wrappers.c  # Non-inline wrappers for inb/outb/etc.
└── kaos_types.h        # ELF structures, kaos_module_info_t, state enum

include/kaos/
├── module.h            # Module author header: KAOS_MODULE macro
├── kernel.h            # Convenience includes for module authors
└── export.h            # KAOS_EXPORT macro
```

---

## Project Structure

```
kernel/kaos/
├── kaos.h              # Public API
├── kaos.c              # Module manager
├── kaos_loader.c       # ELF ET_REL loader
├── kaos_sym.c          # Kernel symbol table
├── kaos_io_wrappers.c  # Wrappers for inline I/O functions
└── kaos_types.h        # All types and ELF definitions

include/kaos/
├── module.h            # KAOS_MODULE, kaos_module_info_t
├── kernel.h            # Convenience includes for modules
└── export.h            # KAOS_EXPORT macro

modules/                # Module source files (built to .kaos)
├── hello.c             # Hello world test module
└── ...
```

---

## Phase 6 Acceptance Tests

1. **Symbol table:** `kaos_sym_count() > 0` after kernel init. `kaos_sym_lookup("kmalloc")` returns non-zero address. `kaos_sym_lookup("nonexistent")` returns 0.
2. **Load hello module:** `kaos_load("/modules/hello.kaos")` succeeds. Serial shows init message. Module state is KAOS_STATE_LOADED. `kaos_find("hello")` returns valid index.
3. **Unload hello module:** `kaos_unload(idx)` succeeds. Serial shows cleanup message. State is KAOS_STATE_UNLOADED. Memory freed (PMM pages returned).
4. **Load module that calls kernel API:** Module does `serial_printf("test")` from `init()`. If the relocation was correct, the output appears. If wrong, it crashes (proves relocations work).
5. **Reject corrupt module:** Load a file that isn't valid ELF. `kaos_load()` returns -1. No crash, error logged.
6. **Reject ABI mismatch:** Load a module with wrong `KAOS_ABI_VERSION`. Rejected at load time with clear error message.
7. **Reject unresolved symbol:** Module references `nonexistent_function()`. Load fails with "unresolved symbol: nonexistent_function" in serial log.
8. **Module with dependency:** Module A depends on module B. Load A — B is loaded first automatically. Unload B fails while A is loaded (dependency check).
9. **Essential module:** Module with `KAOS_FLAG_ESSENTIAL`. `kaos_unload()` returns -1 with error.
10. **Init failure:** Module's `init()` returns -1. Module state is LOAD_FAILED. Memory freed. `cleanup()` NOT called.
11. **Memory cleanup:** Load and unload 10 modules. PMM free page count returns to baseline. No memory leak.
12. **modlist:** After loading 3 modules, `modlist` shell command shows all 3 with correct names, versions, and states.
13. **Auto-load:** Place hello.kaos in `/modules/` with KAOS_FLAG_AUTOLOAD. `kaos_load_all("/modules/")` loads it automatically. Serial shows init message.
14. **Lua binding module:** (When Lua is available) Module calls `lua_register()` from `init()`. Lua script can call the registered function. No crash.
15. **Max modules:** Load 32 modules (KAOS_MAX_MODULES). 33rd load returns -1 with "module table full" error.

---

## Summary

| Property | Value |
|----------|-------|
| File extension | `.kaos` |
| ELF type | ET_REL (relocatable) |
| Relocation types | R_386_32, R_386_PC32 |
| Module info symbol | `kaos_module_info` (kaos_module_info_t) |
| Module magic | 0x4B414F53 ('KAOS') |
| ABI version | 1 (incremented on breaking changes) |
| Max modules | 32 |
| Memory allocator | PMM (PAGE_RESERVED) |
| Symbol lookup | Linear scan of `.kaos_export` section |
| Dependency resolution | Recursive, circular detection |
| Unload safety | Manual (cleanup must deregister all callbacks) |
| Essential flag | KAOS_FLAG_ESSENTIAL — cannot be unloaded |
| Auto-load | KAOS_FLAG_AUTOLOAD — loaded at boot from /modules/ |
| Module CFLAGS | Same as kernel (-mno-sse, -c, no linking) |
| Shell commands | `modload`, `modunload`, `modlist` |
