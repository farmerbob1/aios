# AIOS

A hobby operating system built from scratch for i686 (32-bit x86). Graphical desktop, Lua scripting, software 3D renderer, web browser, networking, and a Doom-style FPS game — all running in ring 0.

Boots via UEFI on QEMU and VirtualBox.

## Screenshots

| Desktop | Settings | ChaosRIP |
|---------|----------|----------|
| ![Desktop](screenshots/desktop.png) | ![Settings](screenshots/settings.png) | ![ChaosRIP](screenshots/chaosrip.png) |

## What's in it

- **UEFI bootloader** — x86_64 PE/COFF app, GOP framebuffer, 64-to-32-bit mode transition
- **Kernel** — PMM, VMM (4KB paging), slab+buddy heap, preemptive scheduler (250Hz), FPU/SSE context switching
- **Drivers** — PS/2 keyboard & mouse (IntelliMouse scroll wheel), ATA DMA (legacy IDE + AHCI/SATA), Intel E1000 NIC, AC97 audio, PCI bus
- **ChaosFS** — custom extent-based filesystem (4KB blocks, inodes, directories, block cache, cross-directory move)
- **ChaosGL** — software 3D renderer, surface compositor, 2D primitives, TrueType fonts, PNG/JPEG textures, SSE2 SIMD
- **KAOS modules** — runtime kernel module loading (.kaos ELF relocatable objects)
- **Lua 5.5** — application scripting with AIOS libraries (filesystem, networking, input, audio, windowing)
- **UI toolkit** — 20+ widgets, flex/grid layout, dark/light themes with live switching
- **Window manager** — macOS-style chrome, floating dock, per-window event queues, boot splash, right-click context menus
- **Desktop** — `/desktop/` folder backed, drag-and-drop icons with grid snapping, clipboard (copy/cut/paste), trash can with restore
- **Networking** — lwIP TCP/IP stack, BearSSL TLS, DHCP, DNS, HTTP client
- **Web browser** — HTML5 parsing (lexbor), CSS styling, image loading, SVG rendering (nanosvg), navigation history
- **Package manager** — install/update/remove apps from a remote repo over HTTPS
- **ChaosRIP** — Doom/Quake-style FPS: portal sectors, sprite enemies, hitscan weapons, 320x200 @ 60fps

### Apps

File Browser (drag-to-folder, context menus, keyboard shortcuts, trash integration), Terminal, Settings, Text Editor (undo/redo, find/replace, Save As dialog), Music Player, Image Viewer, 3D Viewer, System Monitor, Web Server, Web Browser, Package Manager, ChaosRIP.

## Building

Requires: i686-elf cross-compiler, x86_64 MinGW GCC, NASM, Make, Python 3, QEMU or VirtualBox.

```bash
export PATH="/c/i686-elf-tools/bin:/c/msys64/usr/bin:/c/msys64/mingw64/bin:$PATH"

make clean && make all    # Build kernel + UEFI bootloader + 512MB GPT disk image
make run                  # Run in QEMU
```

Or double-click `vm.bat` to run in VirtualBox.

## Terminal

```
ls [path]       cat <file>      cd <dir>        mkdir <name>    rm <path>
mem             clear           help
ifconfig        ping <host>     dns <host>      get <url>
pkg install/remove/update/search/list/info/refresh
```

Anything else is evaluated as Lua.

## Project Structure

```
boot/           UEFI bootloader
kernel/         Core (PMM, VMM, heap, scheduler, interrupts)
  chaos/        ChaosFS
  kaos/         Module system
  lua/          Lua runtime + AIOS bindings
  html/         HTML layout engine (lexbor integration)
  js/           JavaScript engine (QuickJS integration)
  net/          lwIP + BearSSL ports
  audio/        WAV, MP3, MIDI playback
drivers/        Serial, keyboard, mouse, ATA, AHCI, PCI
renderer/       ChaosGL (3D pipeline, compositor, fonts, textures, SVG)
modules/        Loadable kernel modules (E1000, AC97)
harddrive/      ChaosFS disk contents
  apps/         All applications
  system/       Desktop, WM, UI toolkit, themes, fonts
vendor/         Lua 5.5, lwIP 2.2, BearSSL 0.6, stb, minimp3, lexbor, QuickJS, nanosvg
tools/          Build tools (disk builder, FS populator, package tools)
```

## License

Hobby project. Third-party components retain their original licenses:
- Lua 5.5.0: MIT License
- lwIP 2.2.0: BSD License
- BearSSL 0.6: MIT License
- stb_image, stb_truetype: MIT License
- Inter font: SIL Open Font License
- minimp3: CC0
- TinySoundFont: MIT License
- lexbor: Apache License 2.0
- QuickJS: MIT License
- nanosvg: zlib License
- picohttpparser: MIT License
