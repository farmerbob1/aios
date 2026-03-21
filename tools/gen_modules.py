#!/usr/bin/env python3
"""gen_modules — Write compiled .kaos modules and synthetic test files into ChaosFS.
Usage: python gen_modules.py <disk_image> <lba_start> <modules_dir>
Run AFTER mkfs_chaos.py and gen_assets.py."""

import struct
import sys
import os

# Import ChaosFS writer infrastructure from gen_assets
sys.path.insert(0, os.path.dirname(__file__))
from gen_assets import ChaosFSWriter

ROOT_INO = 1


def make_corrupt_kaos():
    """64 bytes of garbage — not a valid ELF."""
    return bytes([0xDE] * 64)


def make_bad_abi_kaos():
    """Minimal valid-looking ELF relocatable with a kaos_module_info that has wrong ABI.

    This is a synthetic ELF — just enough structure to pass header validation
    but contain a kaos_module_info with abi_version = 99."""

    # Build a minimal ET_REL ELF with:
    #   - ELF header (52 bytes)
    #   - Section header table (3 entries: null + .data + .symtab + .strtab = 4 entries)
    #   - .data section containing kaos_module_info_t
    #   - .symtab with one GLOBAL symbol "kaos_module_info"
    #   - .strtab with the symbol name

    # kaos_module_info_t layout (40 bytes on i386):
    #   uint32 magic          = 0x4B414F53
    #   uint32 abi_version    = 99 (WRONG!)
    #   uint32 name_ptr       = 0 (NULL)
    #   uint32 version_ptr    = 0 (NULL)
    #   uint32 author_ptr     = 0 (NULL)
    #   uint32 description_ptr= 0 (NULL)
    #   uint32 init_ptr       = 0 (NULL)
    #   uint32 cleanup_ptr    = 0 (NULL)
    #   uint32 deps_ptr       = 0 (NULL)
    #   uint32 flags          = 0
    mod_info = struct.pack('<IIIIIIIIII',
        0x4B414F53,  # magic
        99,          # abi_version (WRONG)
        0, 0, 0, 0,  # name, version, author, description
        0, 0,        # init, cleanup
        0,           # dependencies
        0,           # flags
    )

    # String table: \0 + "kaos_module_info\0"
    strtab = b'\x00kaos_module_info\x00'

    # Symbol table entry for kaos_module_info (16 bytes)
    # st_name=1, st_value=0, st_size=40, st_info=0x11 (GLOBAL|OBJECT), st_other=0, st_shndx=1 (.data)
    sym_null = struct.pack('<IIIBBH', 0, 0, 0, 0, 0, 0)  # null symbol
    sym_info = struct.pack('<IIIBBH', 1, 0, 40, 0x11, 0, 1)  # kaos_module_info
    symtab = sym_null + sym_info

    # Layout:
    #   0x00: ELF header (52 bytes)
    #   0x34: .data section (mod_info, 40 bytes) — padded to 64
    #   0x74: .strtab (strtab) — padded to 32
    #   0x94: .symtab (symtab, 32 bytes)
    #   0xB4: Section headers (4 * 40 = 160 bytes)

    data_off = 52
    data_size = len(mod_info)
    strtab_off = data_off + 64
    strtab_size = len(strtab)
    symtab_off = strtab_off + 32
    symtab_size = len(symtab)
    shdr_off = symtab_off + symtab_size

    # ELF header
    ehdr = bytearray(52)
    ehdr[0:4] = b'\x7fELF'
    ehdr[4] = 1   # ELFCLASS32
    ehdr[5] = 1   # ELFDATA2LSB
    ehdr[6] = 1   # EV_CURRENT
    struct.pack_into('<HH', ehdr, 16, 1, 3)    # e_type=ET_REL, e_machine=EM_386
    struct.pack_into('<I', ehdr, 20, 1)          # e_version
    struct.pack_into('<I', ehdr, 32, shdr_off)   # e_shoff
    struct.pack_into('<H', ehdr, 40, 52)         # e_ehsize
    struct.pack_into('<H', ehdr, 46, 40)         # e_shentsize
    struct.pack_into('<H', ehdr, 48, 4)          # e_shnum (null + .data + .strtab + .symtab)
    struct.pack_into('<H', ehdr, 50, 0)          # e_shstrndx (not used)

    # Section headers (4 entries, each 40 bytes)
    shdrs = bytearray(4 * 40)

    # [0] null section header (already zeroed)

    # [1] .data — SHT_PROGBITS, SHF_ALLOC
    sh1_off = 40
    struct.pack_into('<IIIIIIIIII', shdrs, sh1_off,
        0,            # sh_name (don't care)
        1,            # sh_type = SHT_PROGBITS
        0x2,          # sh_flags = SHF_ALLOC
        0,            # sh_addr
        data_off,     # sh_offset
        data_size,    # sh_size
        0, 0,         # sh_link, sh_info
        4,            # sh_addralign
        0,            # sh_entsize
    )

    # [2] .strtab — SHT_STRTAB
    sh2_off = 80
    struct.pack_into('<IIIIIIIIII', shdrs, sh2_off,
        0, 3, 0, 0,
        strtab_off, strtab_size,
        0, 0, 1, 0,
    )

    # [3] .symtab — SHT_SYMTAB, sh_link=2 (strtab), sh_info=1 (first global)
    sh3_off = 120
    struct.pack_into('<IIIIIIIIII', shdrs, sh3_off,
        0, 2, 0, 0,
        symtab_off, symtab_size,
        2, 1,        # sh_link=strtab index, sh_info=first global sym index
        4, 16,       # sh_addralign, sh_entsize=sizeof(Elf32_Sym)
    )

    # Assemble
    elf = bytearray(ehdr)
    # Pad to data_off
    elf += mod_info + b'\x00' * (64 - len(mod_info))
    elf += strtab + b'\x00' * (32 - len(strtab))
    elf += symtab
    elf += shdrs

    return bytes(elf)


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <disk_image> <lba_start> <modules_dir>",
              file=sys.stderr)
        sys.exit(1)

    image_path = sys.argv[1]
    lba_start = int(sys.argv[2])
    modules_dir = sys.argv[3]

    with open(image_path, 'r+b') as f:
        writer = ChaosFSWriter(f, lba_start)

        # Create /modules/ directory
        print("gen_modules: creating /modules/")
        modules_ino = writer.create_directory(ROOT_INO, "modules")

        # Write compiled .kaos modules
        kaos_files = sorted([
            fn for fn in os.listdir(modules_dir)
            if fn.endswith('.kaos')
        ]) if os.path.isdir(modules_dir) else []

        for fn in kaos_files:
            filepath = os.path.join(modules_dir, fn)
            with open(filepath, 'rb') as mf:
                data = mf.read()
            writer.create_file(modules_ino, fn, data)
            print(f"gen_modules: wrote /modules/{fn} ({len(data)} bytes)")

        # Generate synthetic test files
        corrupt_data = make_corrupt_kaos()
        writer.create_file(modules_ino, "corrupt.kaos", corrupt_data)
        print(f"gen_modules: wrote /modules/corrupt.kaos ({len(corrupt_data)} bytes)")

        bad_abi_data = make_bad_abi_kaos()
        writer.create_file(modules_ino, "bad_abi.kaos", bad_abi_data)
        print(f"gen_modules: wrote /modules/bad_abi.kaos ({len(bad_abi_data)} bytes)")

        writer.flush()

    print(f"gen_modules: done ({writer.blocks_allocated} blocks, "
          f"{writer.inodes_allocated} inodes allocated)")


if __name__ == '__main__':
    main()
