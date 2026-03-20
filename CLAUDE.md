# AIOS v2 — Claude Code Project Guide

## Toolchain Locations (EXACT PATHS — do not search for these)

| Tool | Path |
|------|------|
| i686-elf-gcc | `/c/i686-elf-tools/bin/i686-elf-gcc` |
| i686-elf-ld | `/c/i686-elf-tools/bin/i686-elf-ld` |
| nasm | `/c/msys64/usr/bin/nasm` |
| make | `/c/msys64/usr/bin/make` |
| QEMU | `/c/Program Files/qemu/qemu-system-x86_64.exe` |
| Python 3 | `python3` (in PATH) |
| Host GCC (mingw64) | `/c/msys64/mingw64/bin/gcc` (has temp dir issues — use Python for host tools) |

## Build Commands

```bash
# Set PATH for build tools
export PATH="/c/i686-elf-tools/bin:/c/msys64/usr/bin:/c/msys64/mingw64/bin:$PATH"

# Build everything (kernel + disk image + format ChaosFS)
make clean && make all

# Run in QEMU (interactive, serial on stdio)
make run

# Run in QEMU headless, capture serial to file (for test verification)
rm -f build/serial.log && timeout 90 "/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cpu core2duo -m 256 -vga std \
  -drive format=raw,file=build/os.img \
  -serial file:build/serial.log \
  -no-reboot -no-shutdown -display none 2>/dev/null
```

## Project Status

Phases 0-4 COMPLETE. 50/50 tests passing. Next: Phase 5 (ChaosGL).

## Disk Image

- `build/os.img` — 512MB
- Sectors 0-2047: Stage1 + Stage2 + Kernel ELF (padded to 1MB)
- Sectors 2048+: ChaosFS region (LBA_START = 2048)
- Formatted by `python3 tools/mkfs_chaos.py build/os.img 2048` during build

## Key Compiler Flags

- **Kernel/drivers**: `-mno-sse -mno-mmx -mno-sse2 -D__AIOS_KERNEL__` (no SSE in interrupt-context code)
- **Renderer (Phase 5+)**: `-msse2 -mfpmath=sse` via `RENDERER_CFLAGS` (SSE2 allowed for SIMD)
- **Host tools**: Use Python, not C (mingw64 gcc has Windows temp directory permission issues)

## Spec Documents

- `documents/AIOS-v2-foundation-spec-revised.md` — Phases 0-3
- `documents/ChaosFS-v2-spec-revised.md` — Phase 4
- `documents/ChaosGL-spec.md` — Phase 5
- `documents/KAOS-module-spec.md` — Phase 6

## Tools

- `tools/mkfs_chaos.py` — Host-side ChaosFS format tool (run during build)
- `tools/chaosfs_explorer.py` — GUI file browser for ChaosFS disk images
- `explorer.bat` — Launches the GUI explorer
- `run.bat` — Launches QEMU

## Architecture Rules

- Framebuffer driver is HAL-only (`fb_get_info`). ALL rendering goes through ChaosGL.
- Timer (PIT 250Hz) and scheduler are in Phase 2, not Phase 3.
- `schedule()` has `pushf/cli` re-entrancy guard and `noinline` + compiler barrier.
- New tasks go through `task_entry_trampoline` which does `sti` before entry function.
- Idle task uses `sti; hlt` (not bare `hlt`).
- ChaosFS inode cache must `chaos_inode_evict()` before `chaos_free_inode()` to prevent ghost writes.
- `chaos_open()` checks for free fd BEFORE creating files on disk.
