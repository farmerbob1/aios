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

Phases 0-10 COMPLETE. 213 tests (211 passing, 2 pre-existing Phase 2 timing flakes).

## Architecture

This is a hobby OS targeting i686 (QEMU `-cpu core2duo`). Everything runs in ring 0, identity-mapped (virt == phys). No userspace, no higher-half kernel.

### Boot Chain
`stage1.asm` (MBR, 512B) → `stage2.asm` (real mode, 8KB: E820, VBE, ELF loader) → `kernel_main()` at physical 0x100000. The `boot_info` struct at 0x10000 passes memory map, framebuffer info, and kernel location to the kernel. Linker script (`linker.ld`) places kernel at 0x100000; entry point is `kernel_main`.

### Kernel Init Order (kernel/main.c)
```
serial_init → vga_init → boot_info validation
→ PMM → VMM → Heap (Phase 1)
→ GDT/TSS → IDT → ISR → IRQ → FPU → Timer → Scheduler (Phase 2)
→ Keyboard → Mouse → Framebuffer HAL → ATA → PCI → ATA DMA (Phase 3)
→ Block Cache → ChaosFS mount (Phase 4)
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
- **ATA**: Bus Master DMA via PCI IDE controller (`drivers/ata_dma.c`). PIO only for IDENTIFY at init. 28-bit LBA, 512-byte sectors. 64KB bounce buffer, PRD table, polled completion.
- **Block Cache** (`kernel/chaos/block_cache.c`): 512-entry LRU write-through cache (2MB). O(1) hash lookup. Sits in `chaos_block.c` between ChaosFS and ATA. Auto-invalidation in `chaos_free_block()`.

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
Runtime kernel module loading. Modules are `.kaos` files (ELF ET_REL relocatable objects) stored on ChaosFS under `/system/modules/`. Key components:
- **Symbol Table** (`kaos_sym.c`): `KAOS_EXPORT()` macro places entries in `.kaos_export` linker section. `kaos_sym_lookup()` scans at runtime. 52 kernel symbols exported.
- **ELF Loader** (`kaos_loader.c`): Parses ET_REL, copies SHF_ALLOC sections to PMM pages, processes R_386_32/R_386_PC32 relocations against kernel symbol table.
- **Module Manager** (`kaos.c`): Registry of up to 32 modules. Handles dependencies (DFS with cycle detection), essential flag, auto-load from `/system/modules/`.
- **Module SDK** (`include/kaos/`): `module.h` (KAOS_MODULE macro), `kernel.h` (convenience includes), `export.h` (KAOS_EXPORT).
- Modules compiled with `MODULE_CFLAGS` (-mno-sse, -c only, no linking). ChaosFS open requires `CHAOS_O_RDONLY` (0x01) flag.

### Lua 5.5 Runtime (Phase 7, `kernel/lua/`)
Embedded Lua 5.5.0 as the application scripting layer. Lua source is unmodified under `vendor/lua-5.5.0/src/`. All adaptation via shim headers (`include/libc/`) and integration layer:
- **Libc Shims** (`include/libc/*.h`): Shadow system headers with minimal implementations for stdio, stdlib, math, setjmp, etc. Types match `include/types.h` to avoid conflicts.
- **setjmp/longjmp** (`lua_setjmp.asm`): i686 implementation saving EBX/ESI/EDI/EBP/ESP/EIP.
- **Math Shim** (`lua_math_shim.c`): Double-precision math (sin, cos, log, exp, pow, etc.) compiled with `RENDERER_CFLAGS` (SSE2). Software polynomial approximations.
- **Core Shim** (`lua_shim.c`): snprintf, strtod, strtol, ctype, locale stubs, FILE stubs, qsort, memory allocator wrappers.
- **State Management** (`lua_state.c`): `lua_state_create()` opens selected libs (base+string+table+math+coroutine+utf8, no io/os/debug), registers AIOS libs, KAOS bindings, print→serial redirect.
- **AIOS Libraries**: `aios.io` (ChaosFS), `aios.os` (timer/sleep/meminfo), `aios.input` (event poll), `aios.task` (spawn/yield/kill), `aios.debug` (serial output).
- **ChaosFS Loader** (`lua_loader.c`): Custom require/dofile/loadfile backed by ChaosFS. Search path: `/system/ui/`, `/system/layout/`, `/system/`, `/apps/`, `/test/`.
- **KAOS Bridge** (`lua_kaos.c`): `kaos_lua_register()`/`kaos_lua_unregister()` for modules to add Lua functions. KAOS_EXPORT'd.
- **Task struct additions**: `lua_state` (void*) and `userdata` (void*) fields added to `struct task`.
- **Three compiler flag sets for Lua**: vendor Lua (`LUA_CFLAGS`, -w suppresses all warnings), kernel integration (`LUA_KERN_CFLAGS`), math shim (`LUA_MATH_CFLAGS`, SSE2).

### UI Toolkit (Phase 8, `harddrive/system/ui/`, `harddrive/system/layout/`)
Lua-based widget library on top of ChaosGL. Pure Lua — no C widgets. Key components:
- **ChaosGL Lua Bindings** (`kernel/lua/lua_chaosgl.c`): Wraps all ChaosGL C functions as `chaos_gl.*` Lua table. Surface, 2D, text, clip, texture, blit, 3D, stats.
- **Core** (`core.lua`): Widget base class, focus chain management, event dispatch, theme loading. Global `theme` table.
- **Widgets** (20 files): Button, Label, TextField, TextArea, Checkbox, Slider, ProgressBar, Panel, ScrollView, ListView, TabView, Window, Menu, Dialog, IconButton, FileItem, AppIcon, Separator, Tooltip, Badge.
- **Layout** (`flex.lua`, `grid.lua`): Flex row/col with spacing, padding, justify, align, flex weights. Grid with configurable cols/rows.
- **Themes** (`/system/themes/dark.lua`, `light.lua`): `return { ... }` tables with ~90 style keys. Widgets read from global `theme` via `get_style()`.
- **Icon Registry** (`icons.lua`): Multi-size icon lookup. Icons are RAWT `.raw` textures generated by `populate_fs.py`.
- **OOP Pattern**: `setmetatable({}, {__index = core.Widget})` inheritance. Widgets implement `draw(x,y)`, `on_input(event)`, `get_size()`.

### Boot Splash (Phase 9, `kernel/boot_splash.c`)
C module drawing a graphical boot screen after ChaosGL init. Mac OS 9-style icon parade during KAOS module loading.
- `boot_splash_init()` — create fullscreen surface (z=50), draw logo
- `boot_splash_status(text)` — update centered status text
- `boot_splash_module(name, loaded, total)` — draw module icon + advance progress bar
- `boot_splash_destroy()` — cleanup (called from desktop.lua via `chaos_gl.boot_splash_destroy()`)

### Window Manager (Phase 9, `kernel/lua/lua_aios_wm.c` + `harddrive/system/wm.lua`)
C-level shared WM registry (`aios.wm.*`) for cross-task window state. Each Lua task gets an isolated `lua_State`, so shared WM state must live in C.
- **C Registry** (`lua_aios_wm.c`): 28 window slots, per-window event queues (256 events), focus/z-order management, maximize/restore geometry.
- **Lua WM** (`wm.lua`): Input routing (hit-test, coord translation), taskbar rendering, cursor surface, app menu, Alt+Tab cycle.
- **Key functions**: `aios.wm.register/unregister/focus/minimize/restore/toggle/maximize/restore_size/get_windows/push_event/poll_event`
- Apps poll events via `aios.wm.poll_event(surface)`, not `aios.input.poll()` directly.

### Desktop & Applications (Phase 10, `harddrive/system/desktop.lua`, `harddrive/apps/`)
- **Desktop shell** (`desktop.lua`): First Lua task spawned. Creates desktop surface (z=0), loads WM, runs event loop routing input to windows.
- **File browser** (`files.lua`): Navigate ChaosFS, list/grid view, toolbar, double-click to open.
- **Settings** (`settings.lua`): 3 tabs — Appearance (theme switch), System (memory/uptime), Modules (KAOS list).
- **Terminal** (`terminal.lua`): Lua REPL + built-in commands (ls, cat, cd, mkdir, rm, mem, clear). Command history.
- **Text editor** (`edit.lua`): Line editing, cursor movement, Ctrl+S save, line numbers, status bar.
- **App pattern**: create surface → `aios.wm.register()` → event loop with `aios.wm.poll_event()` → `aios.wm.unregister()` → destroy surface.

### CPM — Chaos Package Manager (Phase 12, `harddrive/system/lib/cpm.lua`, `harddrive/apps/cpm/`)
- **Core library** (`/system/lib/cpm.lua`): refresh, install, uninstall, update, search, info. Downloads `.cpk` from GitHub Pages repo via HTTPS, extracts to `/apps/<name>/`, tracks in `/system/cpm/installed.lua`.
- **GUI app** (`/apps/cpm/`): Browse/Installed/Settings tabs. Refresh fetches repo index, Install/Update/Remove buttons.
- **Terminal commands**: `pkg refresh|install|remove|update|search|list|info` in terminal app.
- **Repository**: Static files on GitHub Pages (`farmerbob1/chaos-repo`). `repo.lua` index + `.cpk` packages. Published via `tools/cpk_publish.py`.
- **Security**: Path containment (packages can only write to `/apps/<name>/`), CRC-32 verification (via CPK), HTTPS transport (BearSSL).
- **`aios.os.version()`**: Returns `"2.0"` for `min_aios` compatibility checks.

### ChaosRIP — FPS Game (Phase 13, `harddrive/apps/rip/`)
- **Doom/Quake hybrid** FPS: portal-based sectors, sprite enemies, hitscan weapons
- **Renders at 320x200** through ChaosGL's public API exclusively — no special access
- **Runtime model construction**: `chaos_gl.model_create()` + `set_vertex/normal/uv/face` for procedural sector geometry
- **Sprite shader** (`renderer/shaders.c`): "sprite" shader with color-key discard, UV sub-rect for sprite sheets, flat lighting
- **Mouse raw mode**: `aios.input.set_raw_mode(true)` + `aios.input.get_mouse_delta()` for FPS turning
- **Audio**: `aios.audio.play(path)` for fire-and-forget WAV sound effects
- **Game modules** (`lib/`): math_util, assets, level, sector, portal, player, collision, entities, ai, billboard, weapons, render, hud
- **Level format**: Lua tables defining sectors (convex polygons with floor/ceil heights), portal edges, zones, entity spawns
- **Assets**: All textures and sounds generated procedurally in `tools/populate_fs.py`
- **App-relative require**: `require("lib/level")` finds `/apps/rip/lib/level.lua`

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
| 7 | `documents/Lua-runtime-spec.md` |
| 8 | `documents/UI-toolkit-spec.md` |
| 9-10 | `documents/WM-and-Desktop-spec.md` |
| 12 | `documents/CPM-spec.md` |
| 13 | `documents/ChaosRIP-game-spec.md` |

Always re-read the relevant spec before implementing a phase. Specs are the source of truth for struct layouts, API signatures, acceptance tests, and design contracts.

## Testing

Tests are in `kernel/phase{1..10}_tests.c`. A `test_runner_main` task (PRIORITY_HIGH) runs Phase 2 → Phase 10 tests sequentially. Phase 1 tests run synchronously before the scheduler starts. Each phase prints `Phase N: X/Y tests passed` to serial. The test runner must complete within 90 seconds (QEMU timeout for headless runs).

## Host Tools

- `tools/populate_fs.py` — unified tool: formats ChaosFS, copies `harddrive/` contents, generates test assets, injects compiled .kaos modules (run automatically by Makefile)
- `tools/chaosfs_explorer.py` — tkinter GUI for browsing/editing ChaosFS images (launch via `explorer.bat`)
- `harddrive/` — static files placed on the ChaosFS disk image verbatim (directory structure preserved)
