/* AIOS v2 — boot_info struct (shared between bootloader ASM and kernel C)
 *
 * DESIGN CONTRACT: boot_info is always at physical address 0x10000.
 * This gives 4KB of space. The kernel knows to find it at this address.
 * This is a fixed invariant, documented here and asserted in code.
 *
 * The NASM bootloader writes to this struct using manual field offsets
 * (defined in boot/stage2.asm). Those offsets MUST match this layout.
 * Any change here requires updating stage2.asm offsets. */

#pragma once

#include "types.h"

#define BOOT_MAGIC           0x434C4F53  /* 'CLOS' */
#define BOOT_INFO_ADDR       0x10000
#define MAX_E820_ENTRIES     32
#define MAX_VBE_MODES        32
#define MAX_KERNEL_SEGMENTS  8

typedef enum {
    INIT_OK = 0,
    INIT_WARN,
    INIT_FAIL,
    INIT_FATAL
} init_result_t;

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;       /* 1=usable, 2=reserved, 3=ACPI reclaimable, 4=ACPI NVS, 5=bad */
    uint32_t acpi;       /* ACPI extended attributes (unused, padding) */
} __attribute__((packed));  /* 24 bytes each */

struct vbe_mode_entry {
    uint16_t width;
    uint16_t height;
    uint16_t pitch;      /* bytes per scanline (may be > width*4 due to padding) */
    uint16_t mode_number;
    uint32_t fb_addr;    /* physical address of linear framebuffer */
    uint8_t  bpp;
    uint8_t  reserved[3];
} __attribute__((packed));  /* 16 bytes each */

struct kernel_segment {
    uint32_t phys_start;  /* page-aligned start of loaded segment */
    uint32_t phys_end;    /* page-aligned end (start + memsz, rounded up) */
} __attribute__((packed));  /* 8 bytes each */

struct boot_info {
    /* Magic + version */
    uint32_t magic;                  /* Must be BOOT_MAGIC */
    uint32_t version;                /* Struct version, currently 1 */

    /* Memory map */
    uint32_t e820_count;
    struct e820_entry e820_entries[MAX_E820_ENTRIES];  /* 32 x 24 = 768 bytes */
    uint32_t max_phys_addr;          /* highest physical address from E820
                                      * (base + length of highest entry).
                                      * NOT total RAM — includes holes, reserved
                                      * regions, and MMIO gaps. Used to size the
                                      * PMM bitmap. */

    /* Kernel location */
    uint32_t kernel_entry;           /* ELF entry point (physical address) */
    uint32_t kernel_phys_start;      /* Always 0x100000 by linker contract */
    uint32_t kernel_phys_end;        /* highest (p_paddr + p_memsz), page-aligned */
    uint32_t kernel_loaded_bytes;    /* sum of p_filesz across all PT_LOAD segments */
    uint32_t kernel_segment_count;
    struct kernel_segment kernel_segments[MAX_KERNEL_SEGMENTS]; /* 8 x 8 = 64 bytes */

    /* VBE framebuffer */
    uint32_t fb_addr;                /* physical address (0 = no framebuffer, text mode) */
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;               /* bytes per scanline */
    uint8_t  fb_bpp;                 /* bits per pixel (32 expected) */
    uint8_t  fb_pad[3];

    /* Available VBE modes (for settings/resolution switching later) */
    uint16_t vbe_mode_count;
    uint16_t vbe_current_mode;
    struct vbe_mode_entry vbe_modes[MAX_VBE_MODES];  /* 32 x 16 = 512 bytes */

    /* Boot flags */
    uint32_t boot_flags;
} __attribute__((packed));

/* Total struct size: ~1420 bytes. Well within the 4KB page at 0x10000. */
_Static_assert(sizeof(struct boot_info) <= 4096, "boot_info exceeds 4KB page!");
