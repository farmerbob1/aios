# AIOS v2

A hobby operating system targeting i686 (32-bit x86), built from scratch with a graphical desktop, Lua scripting, modular kernel drivers, and a full networking stack.

Runs in QEMU. Everything executes in ring 0 with identity-mapped memory.

## Features

**Kernel (Phases 0-3)**
- Custom bootloader (stage1 MBR + stage2 ELF loader)
- Physical memory manager (bitmap-based, next-fit)
- Virtual memory manager (4KB paging, identity-mapped)
- Kernel heap (slab allocator + buddy allocator)
- Preemptive scheduler (PIT-driven at 250Hz, 32 tasks max)
- GDT/IDT/ISR/IRQ, FPU/SSE context switching
- PS/2 keyboard and mouse drivers
- ATA PIO disk driver

**ChaosFS (Phase 4)**
- Custom extent-based filesystem at LBA 2048
- 4KB blocks, inode-based, directory support
- Full POSIX-like API (open, read, write, seek, stat, mkdir, unlink)

**ChaosGL (Phase 5)**
- Software 3D renderer with surface-based compositor
- Dirty-region tracking for efficient redraws
- 2D primitives, text rendering, texture support, clipping
- SSE2 SIMD inner loops (protected by fxsave/fxrstor)

**KAOS Modules (Phase 6)**
- Runtime kernel module loading from `.kaos` ELF relocatable objects
- Symbol table with 66+ exported kernel functions
- Dependency resolution, essential flag, auto-load from `/system/modules/`

**Lua 5.5 Runtime (Phase 7)**
- Embedded Lua 5.5.0 as the application scripting layer
- Custom libc shims, ChaosFS-backed `require`/`dofile`
- AIOS libraries: `aios.io`, `aios.os`, `aios.input`, `aios.task`, `aios.debug`, `aios.net`

**UI Toolkit (Phase 8)**
- 20 Lua widgets (Button, TextField, TextArea, ListView, TabView, Dialog, etc.)
- Flex and grid layout engines
- Dark/light theme support

**Window Manager & Desktop (Phases 9-10)**
- C-level shared WM registry with per-window event queues
- Mac OS-style floating dock taskbar
- Boot splash with icon parade during module loading
- File browser (grid/list views, sortable, app detection)
- Settings app (appearance, system info, modules)
- Terminal (Lua REPL + built-in commands)
- Text editor with line numbers

**Networking (Phase 11)**
- PCI bus enumeration driver
- Intel E1000 NIC driver (KAOS module) with DMA descriptor rings
- lwIP 2.2.0 TCP/IP stack (DHCP, DNS, TCP, UDP, ICMP, ARP)
- BearSSL 0.6 TLS with HMAC_DRBG PRNG and entropy collection
- Lua networking API (`aios.net.*`) with HTTP client library

## Building

Requires an i686-elf cross-compiler, NASM, Make, Python 3, and QEMU.

```bash
# Set toolchain paths
export PATH="/c/i686-elf-tools/bin:/c/msys64/usr/bin:$PATH"

# Build everything (kernel + 512MB disk image + ChaosFS)
make clean && make all

# Run in QEMU
make run
```

## Terminal Commands

Open the Terminal app from the desktop dock, then:

```
help                    Show available commands
ifconfig                Show network configuration (IP, mask, gateway, MAC)
ping <host>             DNS resolve + TCP reachability test
dns <host>              Resolve hostname to IP address
get <url>               HTTP GET request (auto-adds http:// if omitted)
ls [path]               List directory contents
cat <file>              Display file contents
cd <dir>                Change directory
mkdir <name>            Create directory
rm <path>               Remove file
mem                     Show memory usage
clear                   Clear terminal output
```

Any other input is evaluated as a Lua expression.

## Architecture

```
boot/stage1.asm (MBR) -> boot/stage2.asm (ELF loader) -> kernel/main.c
    |
    v
PMM -> VMM -> Heap -> GDT/IDT -> Scheduler -> Drivers
    |
    v
ChaosFS -> ChaosGL -> KAOS modules -> Lua runtime -> Desktop shell
    |
    v
PCI -> E1000 (KAOS) -> lwIP -> BearSSL -> aios.net.* -> Lua apps
```

## Disk Image Layout

| Region | Offset | Contents |
|--------|--------|----------|
| Stage 1 | Sector 0 | MBR bootloader (512B) |
| Stage 2 | Sectors 1-16 | Second-stage loader (8KB) |
| Kernel | Sector 17+ | ELF binary (~1MB) |
| ChaosFS | LBA 2048+ | Filesystem (rest of 512MB image) |

## Project Structure

```
boot/           Bootloader (stage1 MBR, stage2 ELF loader)
kernel/         Kernel core (PMM, VMM, heap, scheduler, interrupts)
  chaos/        ChaosFS filesystem
  kaos/         KAOS module system
  lua/          Lua runtime integration and AIOS bindings
  net/          Networking (lwIP port, BearSSL port, Lua net API)
drivers/        Hardware drivers (serial, keyboard, mouse, ATA, PCI)
renderer/       ChaosGL software renderer and compositor
modules/        KAOS module source (e1000.c)
include/        Shared headers (types, io, boot_info, kaos SDK)
  libc/         Libc shim headers for vendor libraries
  kaos/         KAOS module SDK headers
vendor/         Third-party source (Lua 5.5, lwIP 2.2, BearSSL 0.6)
harddrive/      Files placed on ChaosFS disk image
  apps/         GUI applications (files, terminal, settings, editor)
  system/       Desktop shell, WM, UI toolkit, themes, icons
    net/        Lua networking library (http.lua)
    modules/    KAOS .kaos binaries (populated at build time)
tools/          Build tools (populate_fs.py, gen_trust_anchors.py)
documents/      Design specifications for each phase
```

## License

Hobby project. Third-party components retain their original licenses:
- Lua 5.5.0: MIT License
- lwIP 2.2.0: BSD License
- BearSSL 0.6: MIT License
