# AIOS v2 Makefile
# Cross-compiler toolchain (kernel — i686 ELF)
CC      = i686-elf-gcc
LD      = i686-elf-ld
AS      = nasm
OBJCOPY = i686-elf-objcopy

# UEFI bootloader toolchain (x86_64 PE/COFF via MinGW)
UEFI_CC = x86_64-w64-mingw32-gcc

# QEMU + UEFI firmware (single pflash — NVRAM not persisted)
QEMU = "/c/Program Files/qemu/qemu-system-x86_64.exe"
OVMF_CODE = /c/Program Files/qemu/share/edk2-x86_64-code.fd

# Directories
BUILDDIR = build
BOOTDIR  = boot
KERNDIR  = kernel
DRVDIR   = drivers
INCDIR   = include

# Kernel/driver code — compiler must NOT emit SSE/MMX/SSE2.
# Reason: IRQ handlers run without fxsave/fxrstor protection. Compiler-emitted
# SSE in kernel code silently clobbers task XMM registers mid-interrupt.
CFLAGS = -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror \
         -O2 -g -std=c11 -march=core2 -mno-sse -mno-mmx -mno-sse2 -I. -D__AIOS_KERNEL__

# Renderer (Phase 5 ChaosGL) — SSE2 allowed for SIMD inner loops.
# ONLY for files under renderer/. Never kernel/, drivers/, or boot/.
RENDERER_CFLAGS = -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror \
                  -O2 -g -std=c11 -march=core2 -msse2 -mfpmath=sse -I. -D__AIOS_KERNEL__
RENDDIR = renderer
MODDIR  = modules

# Module compilation — same target arch as kernel, -c only (no linking)
MODULE_CFLAGS = -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror \
                -O2 -g -std=c11 -march=core2 -mno-sse -mno-mmx -mno-sse2 \
                -Iinclude -c

# Lua vendor source — compiled with kernel CFLAGS + libc shims
# -Iinclude/libc FIRST so our shim headers shadow the compiler's builtins
# -Wno-* flags suppress warnings in Lua's intentional style patterns
LUADIR = vendor/lua-5.5.0/src
# Vendor Lua: suppress ALL warnings — this is unmodified third-party code
LUA_CFLAGS = -ffreestanding -nostdlib -fno-builtin \
             -O2 -g -std=c11 -march=core2 -mno-sse -mno-mmx -mno-sse2 \
             -D__AIOS_KERNEL__ -Iinclude/libc -I$(LUADIR) -Ikernel/lua -I. -w

# Lua kernel integration — same as kernel CFLAGS + Lua headers
LUA_KERN_CFLAGS = $(CFLAGS) -Iinclude/libc -I$(LUADIR) -Ikernel/lua \
                  -Wno-unused-parameter

# Lua math shim — SSE2 enabled for hardware sqrt and fast float ops
LUA_MATH_CFLAGS = $(RENDERER_CFLAGS) -Iinclude/libc -I$(LUADIR) -Ikernel/lua \
                  -Wno-unused-parameter -Wno-unused-variable

# lwIP vendor source — compiled like Lua vendor (suppress all warnings)
LWIPDIR = vendor/lwip-2.2.0/src
LWIP_CFLAGS = -ffreestanding -nostdlib -fno-builtin \
              -O2 -g -std=c11 -march=core2 -mno-sse -mno-mmx -mno-sse2 \
              -D__AIOS_KERNEL__ -Ikernel/net -Ikernel/net/arch -I$(LWIPDIR)/include -Iinclude/libc -I. \
              -w -Wno-implicit-function-declaration

# lwIP kernel integration — same as kernel CFLAGS + lwIP headers
LWIP_KERN_CFLAGS = $(CFLAGS) -Ikernel/net -I$(LWIPDIR)/include -Ikernel/net/arch \
                   -Wno-unused-parameter -Wno-unused-variable

# BearSSL vendor source — pure C89, no FP, no OS deps
BSSLDIR = vendor/bearssl-0.6
BSSL_CFLAGS = -ffreestanding -nostdlib -fno-builtin \
              -O2 -g -std=c99 -march=core2 -mno-sse -mno-mmx -mno-sse2 \
              -D__AIOS_KERNEL__ -I$(BSSLDIR)/inc -I$(BSSLDIR)/src -Iinclude/libc -I. -w

# BearSSL kernel integration — include libc shims FIRST to shadow compiler headers
BSSL_KERN_CFLAGS = $(CFLAGS) -Iinclude/libc -I$(BSSLDIR)/inc -I$(BSSLDIR)/src \
                   -Ikernel/net -I$(LWIPDIR)/include -Ikernel/net/arch \
                   -Wno-unused-parameter -Wno-unused-variable

# Assembler flags
NASMFLAGS_BIN = -f bin
NASMFLAGS_ELF = -f elf32 -g
NASMFLAGS_WIN64 = -f win64

# UEFI bootloader flags (x86_64 PE/COFF, freestanding, MS ABI default)
UEFI_CFLAGS  = -ffreestanding -fno-stack-protector -mno-red-zone -fshort-wchar \
               -Wall -Wextra -O2 -std=c11 -Iboot
UEFI_LDFLAGS = -nostdlib -Wl,--subsystem,10 -e efi_main

# Host tools
PYTHON = python3

# Linker flags
LDFLAGS = -T linker.ld -nostdlib

# ===== Source files =====
C_SOURCES = \
    $(KERNDIR)/main.c \
    $(KERNDIR)/panic.c \
    $(KERNDIR)/boot_display.c \
    $(KERNDIR)/pmm.c \
    $(KERNDIR)/vmm.c \
    $(KERNDIR)/heap.c \
    $(KERNDIR)/slab.c \
    $(KERNDIR)/buddy.c \
    $(KERNDIR)/gdt.c \
    $(KERNDIR)/idt.c \
    $(KERNDIR)/isr.c \
    $(KERNDIR)/irq.c \
    $(KERNDIR)/fpu.c \
    $(KERNDIR)/rdtsc.c \
    $(KERNDIR)/scheduler.c \
    $(DRVDIR)/serial.c \
    $(DRVDIR)/vga.c \
    $(DRVDIR)/timer.c \
    $(DRVDIR)/framebuffer.c \
    $(DRVDIR)/keyboard.c \
    $(DRVDIR)/mouse.c \
    $(DRVDIR)/input.c \
    $(DRVDIR)/ata.c \
    $(DRVDIR)/ata_dma.c \
    $(DRVDIR)/ahci.c \
    $(DRVDIR)/pci.c \
    $(KERNDIR)/net/netbuf.c \
    $(KERNDIR)/net/netif_bridge.c \
    $(KERNDIR)/audio/audio_bridge.c \
    $(KERNDIR)/audio/wav_decode.c \
    $(KERNDIR)/audio/resample.c \
    $(KERNDIR)/audio/midi_parse.c \
    $(KERNDIR)/chaos/chaos_block.c \
    $(KERNDIR)/chaos/block_cache.c \
    $(KERNDIR)/chaos/chaos_alloc.c \
    $(KERNDIR)/chaos/chaos_inode.c \
    $(KERNDIR)/chaos/chaos_format.c \
    $(KERNDIR)/chaos/chaos_dir.c \
    $(KERNDIR)/chaos/chaos.c \
    $(KERNDIR)/chaos/chaos_fsck.c \
    $(KERNDIR)/phase1_tests.c \
    $(KERNDIR)/phase2_tests.c \
    $(KERNDIR)/phase3_tests.c \
    $(KERNDIR)/phase4_tests.c \
    $(KERNDIR)/phase5_tests.c \
    $(KERNDIR)/phase6_tests.c \
    $(KERNDIR)/phase7_tests.c \
    $(KERNDIR)/phase8_tests.c \
    $(KERNDIR)/phase9_tests.c \
    $(KERNDIR)/phase10_tests.c \
    $(KERNDIR)/boot_splash.c \
    $(KERNDIR)/kaos/kaos.c \
    $(KERNDIR)/kaos/kaos_loader.c \
    $(KERNDIR)/kaos/kaos_sym.c \
    $(KERNDIR)/kaos/kaos_io_wrappers.c \
    $(KERNDIR)/compression/lz4.c \
    $(KERNDIR)/compression/cpk.c \
    $(INCDIR)/string.c \
    $(RENDDIR)/compositor.c

# Renderer sources — compiled with RENDERER_CFLAGS (SSE2 enabled)
RENDERER_SOURCES = \
    $(RENDDIR)/math.c \
    $(RENDDIR)/font.c \
    $(RENDDIR)/surface.c \
    $(RENDDIR)/2d.c \
    $(RENDDIR)/pipeline.c \
    $(RENDDIR)/rasterizer.c \
    $(RENDDIR)/shaders.c \
    $(RENDDIR)/texture.c \
    $(RENDDIR)/model.c \
    $(RENDDIR)/chaos_gl.c

# Kernel ASM sources (ELF format, linked into kernel)
ASM_SOURCES = \
    $(KERNDIR)/isr_stubs.asm \
    $(KERNDIR)/irq_stubs.asm \
    $(KERNDIR)/scheduler_asm.asm

# ===== Lua sources =====

# Lua vendor — exclude lua.c (standalone interpreter) and luac.c (compiler CLI)
LUA_VENDOR_SOURCES = $(filter-out $(LUADIR)/lua.c $(LUADIR)/luac.c, \
                     $(wildcard $(LUADIR)/l*.c))

# Lua kernel integration sources
LUA_KERNEL_SOURCES = \
    $(KERNDIR)/lua/lua_shim.c \
    $(KERNDIR)/lua/lua_init.c \
    $(KERNDIR)/lua/lua_state.c \
    $(KERNDIR)/lua/lua_task.c \
    $(KERNDIR)/lua/lua_kaos.c \
    $(KERNDIR)/lua/lua_loader.c \
    $(KERNDIR)/lua/lua_aios_io.c \
    $(KERNDIR)/lua/lua_aios_os.c \
    $(KERNDIR)/lua/lua_aios_input.c \
    $(KERNDIR)/lua/lua_aios_task.c \
    $(KERNDIR)/lua/lua_aios_debug.c \
    $(KERNDIR)/lua/lua_chaosgl.c \
    $(KERNDIR)/lua/lua_aios_wm.c \
    $(KERNDIR)/lua/lua_aios_audio.c \
    $(KERNDIR)/lua/lua_aios_cpk.c

# ===== lwIP sources =====

# lwIP vendor — core + ipv4 + api + ethernet netif
LWIP_VENDOR_SOURCES = $(wildcard $(LWIPDIR)/core/*.c) \
                      $(wildcard $(LWIPDIR)/core/ipv4/*.c) \
                      $(wildcard $(LWIPDIR)/api/*.c) \
                      $(LWIPDIR)/netif/ethernet.c

# lwIP kernel integration sources
LWIP_KERNEL_SOURCES = \
    $(KERNDIR)/net/sys_arch.c \
    $(KERNDIR)/net/lwip_netif.c \
    $(KERNDIR)/net/lwip_init.c \
    $(KERNDIR)/net/lua_net.c

# ===== BearSSL sources =====

BSSL_VENDOR_SOURCES = $(wildcard $(BSSLDIR)/src/codec/*.c) \
                      $(wildcard $(BSSLDIR)/src/ec/*.c) \
                      $(wildcard $(BSSLDIR)/src/hash/*.c) \
                      $(wildcard $(BSSLDIR)/src/int/*.c) \
                      $(wildcard $(BSSLDIR)/src/kdf/*.c) \
                      $(wildcard $(BSSLDIR)/src/mac/*.c) \
                      $(wildcard $(BSSLDIR)/src/rand/*.c) \
                      $(wildcard $(BSSLDIR)/src/rsa/*.c) \
                      $(wildcard $(BSSLDIR)/src/ssl/*.c) \
                      $(wildcard $(BSSLDIR)/src/symcipher/*.c) \
                      $(wildcard $(BSSLDIR)/src/x509/*.c) \
                      $(wildcard $(BSSLDIR)/src/aead/*.c) \
                      $(BSSLDIR)/src/settings.c

BSSL_KERNEL_SOURCES = \
    $(KERNDIR)/net/entropy.c \
    $(KERNDIR)/net/bearssl_port.c \
    $(KERNDIR)/net/trust_anchors.c

# ===== Object files =====
C_OBJECTS        = $(patsubst %.c,$(BUILDDIR)/%.o,$(C_SOURCES))
RENDERER_OBJECTS = $(patsubst %.c,$(BUILDDIR)/%.o,$(RENDERER_SOURCES))
ASM_OBJECTS      = $(patsubst %.asm,$(BUILDDIR)/%.o,$(ASM_SOURCES))

# Lua objects
LUA_VENDOR_OBJECTS  = $(patsubst $(LUADIR)/%.c,$(BUILDDIR)/$(LUADIR)/%.o,$(LUA_VENDOR_SOURCES))
LUA_KERNEL_OBJECTS  = $(patsubst $(KERNDIR)/lua/%.c,$(BUILDDIR)/$(KERNDIR)/lua/%.o,$(LUA_KERNEL_SOURCES))
LUA_MATH_OBJECT     = $(BUILDDIR)/$(KERNDIR)/lua/lua_math_shim.o
LUA_ASM_OBJECT      = $(BUILDDIR)/$(KERNDIR)/lua/lua_setjmp.o

# lwIP objects
LWIP_VENDOR_OBJECTS = $(patsubst $(LWIPDIR)/%.c,$(BUILDDIR)/$(LWIPDIR)/%.o,$(LWIP_VENDOR_SOURCES))
LWIP_KERNEL_OBJECTS = $(patsubst $(KERNDIR)/net/%.c,$(BUILDDIR)/$(KERNDIR)/net/%.o,$(LWIP_KERNEL_SOURCES))

# BearSSL objects
BSSL_VENDOR_OBJECTS = $(patsubst $(BSSLDIR)/%.c,$(BUILDDIR)/$(BSSLDIR)/%.o,$(BSSL_VENDOR_SOURCES))
BSSL_KERNEL_OBJECTS = $(patsubst $(KERNDIR)/net/%.c,$(BUILDDIR)/$(KERNDIR)/net/%.o,$(BSSL_KERNEL_SOURCES))

# ===== Audio decoder sources (special CFLAGS) =====

# minimp3 — no SSE (uses x87 float), suppress vendor warnings
MP3_CFLAGS = -ffreestanding -nostdlib -fno-builtin \
             -O2 -g -std=c11 -march=core2 -mno-sse -mno-mmx -mno-sse2 \
             -D__AIOS_KERNEL__ -Ivendor/minimp3 -Iinclude/libc -I. -w

MP3_OBJECT = $(BUILDDIR)/$(KERNDIR)/audio/mp3_decode.o

# TinySoundFont — SSE2 for float math, only called from task context
TSF_CFLAGS = $(RENDERER_CFLAGS) -Ivendor/tsf -Iinclude/libc -I. -w

TSF_OBJECT = $(BUILDDIR)/$(KERNDIR)/audio/midi_render.o

# stb_image — SSE2 for float math (PNG gamma, JPEG DCT), suppress vendor warnings
STB_CFLAGS = $(RENDERER_CFLAGS) -Ivendor/stb -Iinclude/libc -I. -w

STB_OBJECT = $(BUILDDIR)/$(RENDDIR)/stb_image_decode.o

# stb_truetype — SSE2 for float rasterization, suppress vendor warnings
STBTT_CFLAGS = $(RENDERER_CFLAGS) -Ivendor/stb -Iinclude/libc -I. -w

STBTT_OBJECT = $(BUILDDIR)/$(RENDDIR)/ttf_font.o

ALL_OBJECTS = $(C_OBJECTS) $(RENDERER_OBJECTS) $(ASM_OBJECTS) \
              $(LUA_VENDOR_OBJECTS) $(LUA_KERNEL_OBJECTS) \
              $(LUA_MATH_OBJECT) $(LUA_ASM_OBJECT) \
              $(LWIP_VENDOR_OBJECTS) $(LWIP_KERNEL_OBJECTS) \
              $(BSSL_VENDOR_OBJECTS) $(BSSL_KERNEL_OBJECTS) \
              $(MP3_OBJECT) $(TSF_OBJECT) $(STB_OBJECT) $(STBTT_OBJECT)

# Module .kaos files (compiled from modules/*.c)
MODULE_SOURCES = $(wildcard $(MODDIR)/*.c)
MODULE_KAOS    = $(patsubst $(MODDIR)/%.c,$(BUILDDIR)/$(MODDIR)/%.kaos,$(MODULE_SOURCES))

# ===== Targets =====

.PHONY: all clean run run-debug

all: $(BUILDDIR)/os.img
	@echo "Build complete: $(BUILDDIR)/os.img"

# UEFI bootloader — C source to object (x86_64 PE/COFF)
$(BUILDDIR)/$(BOOTDIR)/uefi_boot.o: $(BOOTDIR)/uefi_boot.c $(BOOTDIR)/uefi.h
	@mkdir -p $(dir $@)
	$(UEFI_CC) $(UEFI_CFLAGS) -c $< -o $@

# UEFI bootloader — 64→32 mode transition assembly
$(BUILDDIR)/$(BOOTDIR)/transition.o: $(BOOTDIR)/transition.asm
	@mkdir -p $(dir $@)
	$(AS) $(NASMFLAGS_WIN64) $< -o $@

# Link UEFI bootloader → BOOTX64.EFI (PE32+ EFI application)
$(BUILDDIR)/BOOTX64.EFI: $(BUILDDIR)/$(BOOTDIR)/uefi_boot.o $(BUILDDIR)/$(BOOTDIR)/transition.o
	@mkdir -p $(dir $@)
	$(UEFI_CC) $(UEFI_LDFLAGS) $^ -o $@
	@echo "UEFI bootloader: $@ ($$(wc -c < $@ | tr -d ' ') bytes)"

# Module source -> .kaos (compiled ET_REL object, no linking)
$(BUILDDIR)/$(MODDIR)/%.kaos: $(MODDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MODULE_CFLAGS) -o $@ $<

# ===== BearSSL compilation rules (must be before generic rules) =====

# BearSSL vendor source -> object
$(BUILDDIR)/$(BSSLDIR)/%.o: $(BSSLDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BSSL_CFLAGS) -c $< -o $@

# BearSSL kernel integration -> object
$(BUILDDIR)/$(KERNDIR)/net/entropy.o: $(KERNDIR)/net/entropy.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/$(KERNDIR)/net/bearssl_port.o: $(KERNDIR)/net/bearssl_port.c
	@mkdir -p $(dir $@)
	$(CC) $(BSSL_KERN_CFLAGS) -c $< -o $@

$(BUILDDIR)/$(KERNDIR)/net/trust_anchors.o: $(KERNDIR)/net/trust_anchors.c
	@mkdir -p $(dir $@)
	$(CC) $(BSSL_KERN_CFLAGS) -c $< -o $@

# ===== Audio decoder compilation rules =====

# minimp3 wrapper — special CFLAGS (no SSE, vendor header warnings suppressed)
$(BUILDDIR)/$(KERNDIR)/audio/mp3_decode.o: $(KERNDIR)/audio/mp3_decode.c
	@mkdir -p $(dir $@)
	$(CC) $(MP3_CFLAGS) -c $< -o $@

# TinySoundFont wrapper — SSE2 for float math
$(BUILDDIR)/$(KERNDIR)/audio/midi_render.o: $(KERNDIR)/audio/midi_render.c
	@mkdir -p $(dir $@)
	$(CC) $(TSF_CFLAGS) -c $< -o $@

# stb_image wrapper — SSE2 for float decode math, suppress vendor warnings
$(BUILDDIR)/$(RENDDIR)/stb_image_decode.o: $(RENDDIR)/stb_image_decode.c
	@mkdir -p $(dir $@)
	$(CC) $(STB_CFLAGS) -c $< -o $@

# stb_truetype wrapper — SSE2 for float rasterization
$(BUILDDIR)/$(RENDDIR)/ttf_font.o: $(RENDDIR)/ttf_font.c
	@mkdir -p $(dir $@)
	$(CC) $(STBTT_CFLAGS) -c $< -o $@

# ===== lwIP compilation rules (must be before generic rules) =====

# lwIP vendor source -> object (suppress all warnings)
$(BUILDDIR)/$(LWIPDIR)/%.o: $(LWIPDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# lwIP kernel integration -> object
$(BUILDDIR)/$(KERNDIR)/net/sys_arch.o: $(KERNDIR)/net/sys_arch.c
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_KERN_CFLAGS) -c $< -o $@

$(BUILDDIR)/$(KERNDIR)/net/lwip_netif.o: $(KERNDIR)/net/lwip_netif.c
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_KERN_CFLAGS) -c $< -o $@

$(BUILDDIR)/$(KERNDIR)/net/lwip_init.o: $(KERNDIR)/net/lwip_init.c
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_KERN_CFLAGS) -c $< -o $@

$(BUILDDIR)/$(KERNDIR)/net/lua_net.o: $(KERNDIR)/net/lua_net.c
	@mkdir -p $(dir $@)
	$(CC) $(LWIP_KERN_CFLAGS) -Iinclude/libc -I$(LUADIR) -Ikernel/lua -c $< -o $@

# ===== Lua compilation rules (must be before generic rules) =====

# Lua vendor source -> object (specific CFLAGS for Lua source)
$(BUILDDIR)/$(LUADIR)/%.o: $(LUADIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

# Lua math shim -> object (SSE2 enabled)
$(LUA_MATH_OBJECT): $(KERNDIR)/lua/lua_math_shim.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_MATH_CFLAGS) -c $< -o $@

# Lua kernel integration -> object
$(BUILDDIR)/$(KERNDIR)/lua/%.o: $(KERNDIR)/lua/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_KERN_CFLAGS) -c $< -o $@

# Lua setjmp assembly -> object
$(LUA_ASM_OBJECT): $(KERNDIR)/lua/lua_setjmp.asm
	@mkdir -p $(dir $@)
	$(AS) $(NASMFLAGS_ELF) $< -o $@

# Phase 7 tests need Lua headers
$(BUILDDIR)/$(KERNDIR)/phase7_tests.o: $(KERNDIR)/phase7_tests.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_KERN_CFLAGS) -c $< -o $@

# Phase 8 tests need Lua headers
$(BUILDDIR)/$(KERNDIR)/phase8_tests.o: $(KERNDIR)/phase8_tests.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_KERN_CFLAGS) -c $< -o $@

# Phase 9 tests need Lua headers
$(BUILDDIR)/$(KERNDIR)/phase9_tests.o: $(KERNDIR)/phase9_tests.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_KERN_CFLAGS) -c $< -o $@

# Phase 10 tests need Lua headers
$(BUILDDIR)/$(KERNDIR)/phase10_tests.o: $(KERNDIR)/phase10_tests.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_KERN_CFLAGS) -c $< -o $@

# ===== Generic compilation rules =====

# Compositor — compiled with kernel CFLAGS (no SSE) so it's safe to call from any task
$(BUILDDIR)/$(RENDDIR)/compositor.o: $(RENDDIR)/compositor.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Renderer source -> object (SSE2 enabled, must be before generic rule)
$(BUILDDIR)/$(RENDDIR)/%.o: $(RENDDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RENDERER_CFLAGS) -c $< -o $@

# C source -> object
$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ASM source -> object (ELF format for linking into kernel)
$(BUILDDIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(NASMFLAGS_ELF) $< -o $@

# Link kernel ELF
# Use $(CC) not $(LD) so it auto-finds libgcc.a for 64-bit arithmetic helpers
$(BUILDDIR)/kernel.elf: $(ALL_OBJECTS) linker.ld
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(ALL_OBJECTS) -o $@ -lgcc

# UEFI disk image assembly
# Layout: GPT + ESP (FAT32, LBA 2048-67583) + ChaosFS (LBA 67584+)
$(BUILDDIR)/os.img: $(BUILDDIR)/BOOTX64.EFI $(BUILDDIR)/kernel.elf $(MODULE_KAOS)
	@echo "Assembling UEFI disk image..."
	$(PYTHON) tools/build_disk.py $(BUILDDIR)/os.img $(BUILDDIR)/BOOTX64.EFI $(BUILDDIR)/kernel.elf
	$(PYTHON) tools/populate_fs.py $(BUILDDIR)/os.img 67584 $(BUILDDIR)/$(MODDIR)
	@echo "Disk image: $(BUILDDIR)/os.img ($$(wc -c < $(BUILDDIR)/os.img | tr -d ' ') bytes)"

# Run in QEMU with UEFI firmware (E1000 NIC, user-mode networking)
# Mouse grab: click inside QEMU window, then Ctrl+Alt+G to capture mouse for FPS games
run: $(BUILDDIR)/os.img
	$(QEMU) \
		-cpu core2duo \
		-m 256 \
		-vga std \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive format=raw,file=$(BUILDDIR)/os.img \
		-netdev user,id=net0,hostfwd=tcp::9090-:9090 \
		-device e1000,netdev=net0 \
		-device AC97,audiodev=audio0 \
		-audiodev sdl,id=audio0,in.voices=0 \
		-serial stdio \
		-no-reboot \
		-no-shutdown

# Run with GDB debug stub (connect with: i686-elf-gdb -ex "target remote :1234")
run-debug: $(BUILDDIR)/os.img
	$(QEMU) \
		-cpu core2duo \
		-m 256 \
		-vga std \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive format=raw,file=$(BUILDDIR)/os.img \
		-netdev user,id=net0,hostfwd=tcp::9090-:9090 \
		-device e1000,netdev=net0 \
		-device AC97,audiodev=audio0 \
		-audiodev sdl,id=audio0,in.voices=0 \
		-serial stdio \
		-no-reboot \
		-no-shutdown \
		-s -S

# Clean build artifacts
clean:
	rm -rf $(BUILDDIR)
