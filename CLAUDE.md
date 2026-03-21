# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
# REQUIRED: set PATH before any make/build commands
export PATH="/c/i686-elf-tools/bin:/c/msys64/usr/bin:/c/msys64/mingw64/bin:$PATH"

# Build everything (kernel + 512MB disk image + format ChaosFS)
make clean && make all

# Run in QEMU (interactive, serial on stdio)
make run

# Run headless and capture serial output for test verification
rm -f build/serial.log && timeout 90 "/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cpu core2duo -m 256 -vga std -drive format=raw,file=build/os.img \
  -serial file:build/serial.log -no-reboot -no-shutdown -display none 2>/dev/null

# Check test results from serial log
grep -E "Phase [0-9]:|accept" build/serial.log
```

## Toolchain Locations (EXACT PATHS — do not search for these)

| Tool | Path |
|------|------|
| i686-elf-gcc | `/c/i686-elf-tools/bin/i686-elf-gcc` |
| i686-elf-ld | `/c/i686-elf-tools/bin/i686-elf-ld` |
| nasm | `/c/msys64/usr/bin/nasm` |
| make | `/c/msys64/usr/bin/make` |
| QEMU | `/c/Program Files/qemu/qemu-system-x86_64.exe` |
| Python 3 | `python3` (in PATH) |
| Host GCC | `/c/msys64/mingw64/bin/gcc` — has Windows temp dir permission issues, use Python for host tools instead |

## Project Status

Phases 0-6 COMPLETE. 111/111 tests passing. Next: Phase 7.

## Architecture

This is a hobby OS targeting i686 (QEMU `-cpu core2duo`). Everything runs in ring 0, identity-mapped (virt == phys). No userspace, no higher-half kernel.

### Boot Chain
`stage1.asm` (MBR, 512B) → `stage2.asm` (real mode, 8KB: E820, VBE, ELF loader) → `kernel_main()` at physical 0x100000. The `boot_info` struct at 0x10000 passes memory map, framebuffer info, and kernel location to the kernel. Linker script (`linker.ld`) places kernel at 0x100000; entry point is `kernel_main`.

### Kernel Init Order (kernel/main.c)
```
serial_init → vga_init → boot_info validation
→ PMM → VMM → Heap (Phase 1)
→ GDT/TSS → IDT → ISR → IRQ → FPU → Timer → Scheduler (Phase 2)
→ Keyboard → Mouse → Framebuffer HAL → ATA (Phase 3)
→ ChaosFS mount (Phase 4)
→ sti → RDTSC calibrate → test_runner task → sleep forever
```

### Memory Subsystem (Phase 1)
- **PMM** (`kernel/pmm.c`): bitmap-based page allocator, next-fit, dynamic bitmap placement
- **VMM** (`kernel/vmm.c`): 4KB page tables, selective identity mapping, pre-allocates all page tables before enabling paging (panics if new PT needed post-paging)
- **Heap** (`kernel/heap.c`): routes to slab (≤2048B) or buddy (>2048B) via `page_ownership[]` table (extern, not static — slab.c needs access)

### Interrupt & Scheduler (Phase 2)
- **GDT** has 5 entries (null, code 0x08, data 0x10, TSS 0x18, reserved). `gdt_set_kernel_stack()` updates TSS esp0 on every context switch.
- **ISR/IRQ stubs** are in NASM (`isr_stubs.asm`, `irq_stubs.asm`). C handlers in `isr.c`/`irq.c`. IRQ sends EOI BEFORE calling handler (critical for task_switch inside timer IRQ).
- **Scheduler** (`kernel/scheduler.c`): `schedule()` has `pushf/cli` re-entrancy guard, `__attribute__((noinline))`, and compiler barrier after `task_switch`. New tasks go through `task_entry_trampoline` (does `sti`) because `schedule()` runs with IF=0. Idle task uses `sti; hlt`.
- **Context switch** (`scheduler_asm.asm`): saves/restores EBP/EBX/ESI/EDI + ESP swap. Args at `[esp+20]` and `[esp+24]` after 4 pushes.

### Drivers (Phase 3)
- **Framebuffer** is HAL-only (`fb_get_info()`). No drawing primitives — ALL rendering goes through ChaosGL (Phase 5).
- **Keyboard**: IRQ1, E0 prefix state machine, scancodes remapped to 128+ for extended keys, dual-mode (GUI events or text-mode ASCII buffer).
- **Mouse**: IRQ12, 3-byte PS/2 packet assembly with sync recovery, desktop mode + raw delta mode.
- **ATA**: PIO mode, 28-bit LBA, 3 retries with soft reset. 512-byte sectors.

### ChaosFS (Phase 4, `kernel/chaos/`)
Extent-based filesystem at LBA 2048 (1MB offset). 4KB blocks (8 sectors each). Key invariants:
- `chaos_inode_evict()` must be called BEFORE `chaos_free_inode()` to prevent cache ghost writes
- `chaos_open()` checks for free fd BEFORE creating files on disk
- Write ordering: data blocks → inode → directory entry. Bitmap is write-through.
- On-disk structs in `chaos_types.h` (shared with host tools via `__AIOS_KERNEL__` guard). Inode = 128 bytes (pad1[32]), dirent = 64 bytes (reserved[4]).

### Disk Image Layout
`build/os.img` is 512MB:
- Sectors 0–2047: Stage1 + Stage2 + Kernel ELF (1MB)
- Sectors 2048+: ChaosFS (formatted and populated by `python3 tools/populate_fs.py` during build)

### KAOS Module System (Phase 6, `kernel/kaos/`)
Runtime kernel module loading. Modules are `.kaos` files (ELF ET_REL relocatable objects) stored on ChaosFS under `/modules/`. Key components:
- **Symbol Table** (`kaos_sym.c`): `KAOS_EXPORT()` macro places entries in `.kaos_export` linker section. `kaos_sym_lookup()` scans at runtime. 52 kernel symbols exported.
- **ELF Loader** (`kaos_loader.c`): Parses ET_REL, copies SHF_ALLOC sections to PMM pages, processes R_386_32/R_386_PC32 relocations against kernel symbol table.
- **Module Manager** (`kaos.c`): Registry of up to 32 modules. Handles dependencies (DFS with cycle detection), essential flag, auto-load from `/modules/`.
- **Module SDK** (`include/kaos/`): `module.h` (KAOS_MODULE macro), `kernel.h` (convenience includes), `export.h` (KAOS_EXPORT).
- Modules compiled with `MODULE_CFLAGS` (-mno-sse, -c only, no linking). ChaosFS open requires `CHAOS_O_RDONLY` (0x01) flag.

## Two Compiler Flag Sets

**Kernel/drivers** (`CFLAGS`): `-mno-sse -mno-mmx -mno-sse2 -D__AIOS_KERNEL__` — prevents compiler from emitting SSE instructions that would corrupt FPU state during interrupts.

**Renderer** (`RENDERER_CFLAGS`): `-msse2 -mfpmath=sse` — SSE2 enabled for ChaosGL SIMD inner loops. ONLY for files under `renderer/`.

The scheduler's `fxsave`/`fxrstor` protects renderer tasks' XMM registers across context switches.

## Spec Documents

| Phase | Spec |
|-------|------|
| 0-3 | `documents/AIOS-v2-foundation-spec-revised.md` |
| 4 | `documents/ChaosFS-v2-spec-revised.md` |
| 5 | `documents/ChaosGL-spec.md` |
| 6 | `documents/KAOS-module-spec.md` |

Always re-read the relevant spec before implementing a phase. Specs are the source of truth for struct layouts, API signatures, acceptance tests, and design contracts.

## Testing

Tests are in `kernel/phase{1..6}_tests.c`. A `test_runner_main` task (PRIORITY_HIGH) runs Phase 2 → Phase 6 tests sequentially. Phase 1 tests run synchronously before the scheduler starts. Each phase prints `Phase N: X/Y tests passed` to serial. The test runner must complete within 90 seconds (QEMU timeout for headless runs).

## Host Tools

- `tools/populate_fs.py` — unified tool: formats ChaosFS, copies `harddrive/` contents, generates test assets, injects compiled .kaos modules (run automatically by Makefile)
- `tools/chaosfs_explorer.py` — tkinter GUI for browsing/editing ChaosFS images (launch via `explorer.bat`)
- `harddrive/` — static files placed on the ChaosFS disk image verbatim (directory structure preserved)
