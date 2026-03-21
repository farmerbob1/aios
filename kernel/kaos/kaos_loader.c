/* kernel/kaos/kaos_loader.c — ELF ET_REL relocatable object loader
 *
 * Loads .kaos modules from ChaosFS, processes relocations against
 * the kernel symbol table, and validates module info. */

#include "kaos_types.h"
#include "kaos.h"
#include "../pmm.h"
#include "../vmm.h"
#include "../heap.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../chaos/chaos.h"

#define KAOS_MAX_SECTIONS 32
#define PAGE_SIZE 4096

#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

/* Read entire file from ChaosFS into a heap buffer */
static uint8_t* read_file(const char* path, uint32_t* out_size) {
    struct chaos_stat st;
    if (chaos_stat(path, &st) != 0) {
        serial_printf("[kaos] file not found: %s\n", path);
        return 0;
    }

    uint32_t size = (uint32_t)st.size;
    if (size < sizeof(Elf32_Ehdr)) {
        serial_printf("[kaos] file too small: %s (%u bytes)\n", path, size);
        return 0;
    }

    uint8_t* buf = kmalloc(size);
    if (!buf) {
        serial_printf("[kaos] out of memory for file: %s\n", path);
        return 0;
    }

    int fd = chaos_open(path, 0x01); /* CHAOS_O_RDONLY */
    if (fd < 0) {
        serial_printf("[kaos] cannot open: %s\n", path);
        kfree(buf);
        return 0;
    }

    int rd = chaos_read(fd, buf, size);
    chaos_close(fd);

    if (rd != (int)size) {
        serial_printf("[kaos] short read: %s (%d/%u)\n", path, rd, size);
        kfree(buf);
        return 0;
    }

    *out_size = size;
    return buf;
}

/* Validate ELF header */
static int validate_elf(const Elf32_Ehdr* ehdr) {
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        serial_printf("[kaos] not an ELF file\n");
        return -1;
    }
    if (ehdr->e_ident[4] != 1) {
        serial_printf("[kaos] not ELF32\n");
        return -1;
    }
    if (ehdr->e_ident[5] != 1) {
        serial_printf("[kaos] not little-endian\n");
        return -1;
    }
    if (ehdr->e_type != ET_REL) {
        serial_printf("[kaos] not relocatable (type=%u)\n", ehdr->e_type);
        return -1;
    }
    if (ehdr->e_machine != EM_386) {
        serial_printf("[kaos] not i386 (machine=%u)\n", ehdr->e_machine);
        return -1;
    }
    return 0;
}

int kaos_loader_load(const char* path, struct kaos_module* mod) {
    uint32_t file_size = 0;
    uint8_t* file_buf = read_file(path, &file_size);
    if (!file_buf) return -1;

    Elf32_Ehdr* ehdr = (Elf32_Ehdr*)file_buf;
    if (validate_elf(ehdr) != 0) {
        kfree(file_buf);
        return -1;
    }

    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        serial_printf("[kaos] no section headers\n");
        kfree(file_buf);
        return -1;
    }

    /* Parse section headers */
    Elf32_Shdr* shdrs = (Elf32_Shdr*)(file_buf + ehdr->e_shoff);
    uint16_t num_sections = ehdr->e_shnum;

    if (num_sections > KAOS_MAX_SECTIONS) {
        serial_printf("[kaos] too many sections (%u)\n", num_sections);
        kfree(file_buf);
        return -1;
    }

    /* Find symtab and strtab */
    Elf32_Sym* symtab = 0;
    char* strtab = 0;
    uint32_t symtab_entries = 0;

    for (uint16_t i = 0; i < num_sections; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab = (Elf32_Sym*)(file_buf + shdrs[i].sh_offset);
            symtab_entries = shdrs[i].sh_size / sizeof(Elf32_Sym);
            /* strtab is at sh_link */
            uint32_t strtab_idx = shdrs[i].sh_link;
            if (strtab_idx < num_sections) {
                strtab = (char*)(file_buf + shdrs[strtab_idx].sh_offset);
            }
            break;
        }
    }

    if (!symtab || !strtab) {
        serial_printf("[kaos] no symbol table found\n");
        kfree(file_buf);
        return -1;
    }

    /* Calculate total size for SHF_ALLOC sections */
    uint32_t total_size = 0;
    for (uint16_t i = 0; i < num_sections; i++) {
        if (!(shdrs[i].sh_flags & SHF_ALLOC)) continue;
        uint32_t align = shdrs[i].sh_addralign;
        if (align > 1) total_size = ALIGN_UP(total_size, align);
        total_size += shdrs[i].sh_size;
    }

    if (total_size == 0) {
        serial_printf("[kaos] no loadable sections\n");
        kfree(file_buf);
        return -1;
    }

    /* Allocate PMM pages */
    uint32_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t load_base = pmm_alloc_pages(pages);
    if (load_base == 0) {
        serial_printf("[kaos] out of memory (%u pages)\n", pages);
        kfree(file_buf);
        return -1;
    }
    heap_mark_reserved(load_base, pages);
    vmm_map_range(load_base, load_base, pages * PAGE_SIZE,
                  PTE_PRESENT | PTE_WRITABLE);
    memset((void*)load_base, 0, pages * PAGE_SIZE);

    /* Copy sections to allocated memory */
    uint32_t section_load_addr[KAOS_MAX_SECTIONS];
    memset(section_load_addr, 0, sizeof(section_load_addr));

    uint32_t offset = 0;
    for (uint16_t i = 0; i < num_sections; i++) {
        if (!(shdrs[i].sh_flags & SHF_ALLOC)) continue;

        uint32_t align = shdrs[i].sh_addralign;
        if (align > 1) offset = ALIGN_UP(offset, align);

        section_load_addr[i] = load_base + offset;

        if (shdrs[i].sh_type == SHT_NOBITS) {
            /* .bss — already zeroed */
        } else {
            memcpy((void*)(load_base + offset),
                   file_buf + shdrs[i].sh_offset,
                   shdrs[i].sh_size);
        }
        offset += shdrs[i].sh_size;
    }

    /* Process relocations */
    for (uint16_t i = 0; i < num_sections; i++) {
        if (shdrs[i].sh_type != SHT_REL) continue;

        uint32_t target_section = shdrs[i].sh_info;
        if (section_load_addr[target_section] == 0) continue;

        Elf32_Rel* rels = (Elf32_Rel*)(file_buf + shdrs[i].sh_offset);
        uint32_t num_rels = shdrs[i].sh_size / sizeof(Elf32_Rel);

        for (uint32_t r = 0; r < num_rels; r++) {
            uint32_t sym_idx = ELF32_R_SYM(rels[r].r_info);
            uint32_t rel_type = ELF32_R_TYPE(rels[r].r_info);

            if (sym_idx >= symtab_entries) {
                serial_printf("[kaos] invalid symbol index %u\n", sym_idx);
                goto fail_free;
            }

            Elf32_Sym* sym = &symtab[sym_idx];
            uint32_t sym_addr;

            if (sym->st_shndx != SHN_UNDEF) {
                /* Symbol defined in module */
                sym_addr = section_load_addr[sym->st_shndx] + sym->st_value;
            } else {
                /* External symbol — look up in kernel */
                const char* sym_name = strtab + sym->st_name;
                sym_addr = kaos_sym_lookup(sym_name);
                if (sym_addr == 0) {
                    serial_printf("[kaos] unresolved symbol: %s\n", sym_name);
                    goto fail_free;
                }
            }

            /* Apply relocation */
            uint32_t* target = (uint32_t*)(section_load_addr[target_section]
                                            + rels[r].r_offset);

            if (rel_type == R_386_32) {
                *target += sym_addr;  /* S + A (addend is existing *target) */
            } else if (rel_type == R_386_PC32) {
                *target += sym_addr - (uint32_t)target;  /* S + A - P */
            } else {
                serial_printf("[kaos] unsupported reloc type %u\n", rel_type);
                goto fail_free;
            }
        }
    }

    /* Find kaos_module_info symbol */
    kaos_module_info_t* info = 0;
    for (uint32_t s = 0; s < symtab_entries; s++) {
        if (ELF32_ST_BIND(symtab[s].st_info) != STB_GLOBAL) continue;
        if (symtab[s].st_shndx == SHN_UNDEF) continue;

        const char* sname = strtab + symtab[s].st_name;
        if (strcmp(sname, "kaos_module_info") == 0) {
            info = (kaos_module_info_t*)
                (section_load_addr[symtab[s].st_shndx] + symtab[s].st_value);
            break;
        }
    }

    if (!info) {
        serial_printf("[kaos] module has no kaos_module_info symbol\n");
        goto fail_free;
    }

    /* Validate module info */
    if (info->magic != KAOS_MODULE_MAGIC) {
        serial_printf("[kaos] bad magic: 0x%08x (expected 0x%08x)\n",
                      info->magic, KAOS_MODULE_MAGIC);
        goto fail_free;
    }
    if (info->abi_version != KAOS_ABI_VERSION) {
        serial_printf("[kaos] ABI mismatch: %u (expected %u)\n",
                      info->abi_version, KAOS_ABI_VERSION);
        goto fail_free;
    }

    /* Success — fill module struct */
    mod->load_base = load_base;
    mod->load_pages = pages;
    mod->info = info;

    kfree(file_buf);
    return 0;

fail_free:
    /* Free allocated pages */
    pmm_free_pages(load_base, pages);
    kfree(file_buf);
    return -1;
}

void kaos_loader_unload(struct kaos_module* mod) {
    if (mod->load_base && mod->load_pages) {
        memset((void*)mod->load_base, 0, mod->load_pages * PAGE_SIZE);
        pmm_free_pages(mod->load_base, mod->load_pages);
        mod->load_base = 0;
        mod->load_pages = 0;
        mod->info = 0;
    }
}
