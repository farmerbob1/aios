# AIOS v2 Makefile
# Cross-compiler toolchain
CC      = i686-elf-gcc
LD      = i686-elf-ld
AS      = nasm
OBJCOPY = i686-elf-objcopy

# QEMU — use x86_64 binary even though our OS is 32-bit.
# Avoids TCG warning about core2duo's long mode flag on i386 emulator.
QEMU = "/c/Program Files/qemu/qemu-system-x86_64.exe"

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

# Renderer module (Phase 9) — SSE2 allowed for SIMD inner loops.
# ONLY for files under renderer/. Never kernel/, drivers/, or boot/.
# Example usage:
#   renderer/rasterizer.o: renderer/rasterizer.c
#       $(CC) $(RENDERER_CFLAGS) -c $< -o $@
RENDERER_CFLAGS = -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror \
                  -O2 -g -std=c11 -march=core2 -msse2 -mfpmath=sse -I.

# Assembler flags
NASMFLAGS_BIN = -f bin
NASMFLAGS_ELF = -f elf32 -g

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
    $(KERNDIR)/chaos/chaos_block.c \
    $(KERNDIR)/chaos/chaos_alloc.c \
    $(KERNDIR)/chaos/chaos_inode.c \
    $(KERNDIR)/chaos/chaos_format.c \
    $(KERNDIR)/chaos/chaos_dir.c \
    $(KERNDIR)/chaos/chaos.c \
    $(KERNDIR)/chaos/chaos_fsck.c \
    $(INCDIR)/string.c

# Kernel ASM sources (ELF format, linked into kernel)
ASM_SOURCES = \
    $(KERNDIR)/isr_stubs.asm \
    $(KERNDIR)/irq_stubs.asm \
    $(KERNDIR)/scheduler_asm.asm

# Object files
C_OBJECTS   = $(patsubst %.c,$(BUILDDIR)/%.o,$(C_SOURCES))
ASM_OBJECTS = $(patsubst %.asm,$(BUILDDIR)/%.o,$(ASM_SOURCES))
ALL_OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

# ===== Targets =====

.PHONY: all clean run run-debug stage2_check

all: $(BUILDDIR)/os.img
	@echo "Build complete: $(BUILDDIR)/os.img"

# Stage 1 (raw binary, 512 bytes)
$(BUILDDIR)/stage1.bin: $(BOOTDIR)/stage1.asm
	@mkdir -p $(dir $@)
	$(AS) $(NASMFLAGS_BIN) $< -o $@

# Stage 2 (raw binary, max 8KB)
$(BUILDDIR)/stage2.bin: $(BOOTDIR)/stage2.asm
	@mkdir -p $(dir $@)
	$(AS) $(NASMFLAGS_BIN) $< -o $@

# Stage 2 size check — MUST be <= 8192 bytes (16 sectors)
stage2_check: $(BUILDDIR)/stage2.bin
	@SIZE=$$(wc -c < $(BUILDDIR)/stage2.bin | tr -d ' '); \
	if [ $$SIZE -gt 8192 ]; then \
		echo "ERROR: Stage 2 is $$SIZE bytes (max 8192)"; exit 1; \
	fi; \
	echo "Stage 2 size: $$SIZE / 8192 bytes"

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

# Disk image assembly
# Layout: sector 0      = stage1 (512B)
#         sectors 1-16  = stage2 (8192B, zero-padded)
#         sector 17+    = kernel ELF (variable size)
$(BUILDDIR)/os.img: $(BUILDDIR)/stage1.bin $(BUILDDIR)/stage2.bin $(BUILDDIR)/kernel.elf stage2_check
	@echo "Assembling disk image..."
	cp $(BUILDDIR)/stage1.bin $(BUILDDIR)/os.img
	dd if=$(BUILDDIR)/stage2.bin of=$(BUILDDIR)/stage2_padded.bin bs=8192 conv=sync 2>/dev/null
	cat $(BUILDDIR)/stage2_padded.bin >> $(BUILDDIR)/os.img
	cat $(BUILDDIR)/kernel.elf >> $(BUILDDIR)/os.img
	@# Pad to 16MB and format ChaosFS at LBA 2048 (1MB offset)
	truncate -s 16M $(BUILDDIR)/os.img
	$(PYTHON) tools/mkfs_chaos.py $(BUILDDIR)/os.img 2048
	@echo "Disk image: $(BUILDDIR)/os.img ($$(wc -c < $(BUILDDIR)/os.img | tr -d ' ') bytes)"

# Run in QEMU
run: $(BUILDDIR)/os.img
	$(QEMU) \
		-cpu core2duo \
		-m 256 \
		-vga std \
		-drive format=raw,file=$(BUILDDIR)/os.img \
		-serial stdio \
		-no-reboot \
		-no-shutdown

# Run with GDB debug stub (connect with: i686-elf-gdb -ex "target remote :1234")
run-debug: $(BUILDDIR)/os.img
	$(QEMU) \
		-cpu core2duo \
		-m 256 \
		-vga std \
		-drive format=raw,file=$(BUILDDIR)/os.img \
		-serial stdio \
		-no-reboot \
		-no-shutdown \
		-s -S

# Clean build artifacts
clean:
	rm -rf $(BUILDDIR)
