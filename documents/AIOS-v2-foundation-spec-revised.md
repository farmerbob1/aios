# AIOS v2 — Foundation Specification (Revised)
## Phases 0–3: Bootloader, Memory, Multitasking, Drivers

---

## Preface: Lessons From v1

AIOS v1 was built fast and broke often. Every subsystem was the bare minimum needed to move to the next phase. This created cascading problems: the bootloader had hardcoded sector counts that broke every time the kernel grew. The memory allocator was a homework-grade first-fit list that degraded under real load. The scheduler had no FPU state management, causing silent numerical corruption between tasks. The VMM used 4MB PSE pages that had to be split at runtime with fragile special-case code. Init functions printed green "OK" even when they failed.

**v2 rules:**
1. **Every init function returns a real status code.** Green means it actually works. Red means it actually failed. No cosmetic lies.
2. **Each phase has acceptance tests.** The phase is not complete until the tests pass. No moving forward on a shaky base.
3. **Design for the final system, not the current phase.** If Lua will eventually hammer the allocator with thousands of allocs/sec, the allocator must handle that from day one — not get patched later.
4. **No undocumented magic numbers.** Any fixed constant must be documented as a design contract with rationale.
5. **Comments explain WHY, not just WHAT.** Every non-obvious design decision gets a comment explaining the reasoning.
6. **Hidden assumptions are bugs.** If the design depends on something (linker address, memory layout, struct size), state it explicitly and assert it.

### Target Platform

**Primary development environment:** QEMU/KVM (i686 emulation).
**Target hardware:** Any x86 PC with BIOS boot, PS/2 input, VBE-capable video, and ATA/IDE storage.

All design decisions must be valid on real bare-metal hardware. QEMU is a development convenience, not a platform contract. Where real hardware behavior differs from QEMU (disk reliability, timing variance, VBE quirks), the spec targets real hardware and notes where QEMU simplifies testing.

---

## Phase 0: Bootloader v2

### Overview

Two-stage bootloader. Stage 1 lives in the MBR (512 bytes), loads Stage 2. Stage 2 runs in real mode, does everything that requires BIOS services (E820 memory map, VBE mode setting, ELF kernel loading), builds a boot info struct, switches to 32-bit protected mode, and jumps to the kernel entry point with a pointer to boot_info.

### Memory Map During Boot

```
0x00000 - 0x004FF   Real mode IVT + BIOS data area (DO NOT TOUCH)
0x00500 - 0x07BFF   Free — used as Stage 2 scratch space (stack, temp buffers)
0x07C00 - 0x07DFF   Stage 1 (loaded by BIOS from MBR)
0x07E00 - 0x09DFF   Stage 2 code (loaded by Stage 1) — max 8KB (16 sectors)
0x09E00 - 0x0FFFF   Free — Stage 2 stack (grows down from 0x0FFFF)
0x10000 - 0x10FFF   boot_info struct (4KB reserved) <- FIXED LOCATION
0x11000 - 0x1FFFF   VBE mode list + scratch (60KB)
0x20000 - 0x9FFFF   ELF header parsing scratch + general real-mode scratch (512KB)
0xA0000 - 0xBFFFF   VGA/MMIO (reserved)
0xC0000 - 0xFFFFF   BIOS ROM area (reserved)
0x100000+           Kernel loaded here (1MB mark)
```

**Design contract: boot_info is always at physical address 0x10000.** This gives 4KB of space — enough for the struct plus embedded arrays. The kernel knows to find it at this address. This is a fixed invariant, documented here and asserted in code.

### Disk Layout

```
Sector 0:           MBR (Stage 1) — 512 bytes
Sectors 1-16:       Stage 2 — fixed 16 sectors (8KB max)
Sector 17+:         Kernel ELF binary — variable size, read from ELF header
```

**Design contract: Stage 2 occupies sectors 1-16 (8KB). This is a fixed invariant.** Stage 2 will not grow beyond 8KB. If it approaches that limit, code must be moved into the kernel instead. This contract is documented here and enforced by a build-time size check in the Makefile:

```makefile
# Build-time check: Stage 2 must fit in 8KB (16 sectors)
stage2_check:
    @SIZE=$$(stat -c%s boot/stage2.bin); \
    if [ $$SIZE -gt 8192 ]; then \
        echo "ERROR: Stage 2 is $$SIZE bytes, max is 8192"; exit 1; \
    fi
```

**The kernel ELF starts at sector 17.** Its size is NOT hardcoded — Stage 2 reads the ELF header to determine how much to load. This is the one truly dynamic part of the boot chain.

### Stage 1 (boot/stage1.asm) — 512 bytes

**Job:** Load Stage 2 from disk into memory and jump to it.

- Load 16 sectors starting from LBA 1 to address 0x7E00 using BIOS INT 13h (AH=0x42, LBA mode)
- On disk read failure: retry up to 3 times with a BIOS disk reset (INT 13h AH=0x00) between attempts. After 3 failures, print error character and halt. Real hardware can fail reads due to spinup timing, DMA boundary issues, or transient controller errors.
- Jump to 0x7E00
- Boot signature 0xAA55 at bytes 510-511

**Implementation notes:**
- Use INT 13h extensions (LBA mode) not CHS — simpler and works for any disk size
- Save the BIOS boot drive number (DL) for Stage 2 to use
- Total code should be under 400 bytes leaving room for a small partition table if needed later

### Stage 2 (boot/stage2.asm) — max 8KB

Stage 2 does everything that needs real mode BIOS access, in this order:

#### Step 1: Save boot drive
Store DL (boot drive from BIOS) for later disk reads.

#### Step 2: E820 Memory Map
- Call INT 15h, EAX=0xE820 in a loop to enumerate memory regions
- Store entries directly into boot_info.e820_entries[] at 0x10000 + offset
- Store count in boot_info.e820_count
- If E820 fails entirely, try INT 15h AX=0xE801 as fallback, construct a synthetic E820 map:
  - E801 returns memory below 16MB (in 1KB blocks, AX/CX) and above 16MB (in 64KB blocks, BX/DX)
  - Construct two E820 entries: one for 0x100000 to 16MB, one for 16MB to (16MB + BX*64KB)
  - Mark both as type 1 (usable). Set e820_count = 2 (plus the mandatory low-memory entry 0x0-0x9FFFF)
  - If E801 also fails: assume 64MB, construct a single usable entry from 0x100000 to 0x4000000. Print warning to serial.
- Compute and store boot_info.max_phys_addr (highest address from any E820 entry: base + length)
- **Max entries: 32** (not 64 — real systems rarely have more than 10-15, 32 is very generous, saves space)

#### Step 3: VBE Mode Setup
- Call VBE function 0x4F00 (Get Controller Info) to verify VBE 2.0+ support
- Enumerate available modes from the mode list pointer
- For each mode, call 0x4F01 (Get Mode Info) and check:
  - Linear framebuffer available (bit 7 of mode attributes)
  - Color depth = 32 bpp
  - Memory model = direct color (type 6)
- Store qualifying modes in boot_info.vbe_modes[] with width, height, pitch, framebuffer address
- **Max stored modes: 32** (plenty for realistic hardware)
- **Mode selection priority:** prefer 1024x768, fall back to 800x600, then 640x480, then highest available
- Set the selected mode with VBE function 0x4F02 (Set Mode) with bit 14 set (linear framebuffer)
- Store selected mode info in boot_info: fb_addr, fb_width, fb_height, fb_pitch, fb_bpp
- **If VBE fails entirely:** fall back to VGA text mode (80x25). Set boot_info.fb_addr = 0 to signal "no framebuffer" to the kernel. The kernel must handle this gracefully.

#### Step 4: Load Kernel ELF

ELF loading uses a **streaming approach**: segments are read directly from disk to their target physical address (p_paddr). The scratch region at 0x20000 is used only for parsing the ELF header and program header table — typically under 1KB total. There is no requirement that the entire kernel fit in the scratch region.

**Real mode addressing constraint:** Real mode cannot directly address physical addresses above 0xFFFFF (1MB). Stage 2 uses **unreal mode** (big real mode) to bridge this gap — briefly entering protected mode to load 32-bit segment descriptors with 4GB limits, then returning to real mode. This gives real mode code 32-bit addressing capability while retaining BIOS interrupt access for INT 13h disk reads. Standard technique used by most production two-stage bootloaders (GRUB, SYSLINUX, etc.). Disk sectors are read to a low-memory scratch buffer via INT 13h, then copied to the target p_paddr above 1MB using 32-bit addressing.

- Read the ELF header (first 512 bytes) from sector 17 into scratch at 0x20000
- **Validate:** magic bytes (0x7F 'E' 'L' 'F'), 32-bit, little-endian, executable
- **Contract: The kernel ELF is linked at physical address 0x100000 (1MB).** The linker script MUST set both p_vaddr and p_paddr to physical addresses starting at 0x100000. The bootloader loads segments to p_paddr. This is a hard contract — if the linker script changes, boot breaks. **Assert: reject ELF if any p_paddr < 0x100000 or > 0x1000000 (16MB sanity limit).**
- Read the program header table into scratch (starting at 0x20000 + 512). **Limit: program header table must fit within scratch space.** In practice, ELF program header tables are under 256 bytes (8 entries x 32 bytes each). The 512KB scratch region is vastly more than needed.
- For each PT_LOAD segment:
  - **Stream segment data directly from disk to p_paddr.** Calculate the starting sector from p_offset, read sectors sequentially to the target physical address. No intermediate buffering in the scratch region.
  - If p_filesz < p_memsz, zero-fill the remainder (BSS)
  - On disk read failure: retry up to 3 times per sector group with BIOS reset between attempts. Halt on persistent failure.
- Store boot_info.kernel_phys_start = 0x100000 (fixed by linker contract; **assert: lowest p_paddr across all PT_LOAD segments must equal 0x100000**)
- **Store boot_info.kernel_phys_end = highest (p_paddr + p_memsz) across all PT_LOAD segments, page-aligned up.** This accounts for non-contiguous segments.
- **Store boot_info.kernel_segments[]: array of {phys_start, phys_end} for each PT_LOAD segment.** The PMM needs this to reserve the actual loaded ranges, not just the bounding box. Max 8 segments (ELF rarely has more than 2-3).
- Store boot_info.kernel_entry = e_entry from ELF header
- Store boot_info.kernel_loaded_bytes = sum of p_filesz across all PT_LOAD segments (bytes actually read from disk, not including BSS zero-fill)

#### Step 5: Build boot_info finalization
- Set boot_info.magic = 0x434C4F53 ('CLOS')
- Set boot_info.boot_flags (debug, verbose, etc. — all zero for now)
- Sanity check: assert all required fields are populated

#### Step 6: Enter Protected Mode
- Disable interrupts (CLI)
- Enable A20 line (fast A20 via port 0x92, fallback to keyboard controller method)
- Load GDT with flat code and data segments (base 0, limit 4GB)
- Set CR0 bit 0 (PE)
- Far jump to 32-bit code segment
- Load data segment registers
- Clear direction flag (CLD)
- Set up a temporary kernel stack at 0x90000 (below kernel load area)
- Push pointer to boot_info (0x10000) as argument
- Call boot_info.kernel_entry

#### Kernel Entry ABI Contract

The exact machine state when `kernel_main` is entered:

| Register / State | Value | Notes |
|---|---|---|
| CS | 0x08 | Flat 32-bit code segment, base 0, limit 4GB |
| DS, ES, FS, GS, SS | 0x10 | Flat 32-bit data segment, base 0, limit 4GB |
| ESP | 0x90000 | Temporary stack (~576KB below kernel at 1MB) |
| EBP | 0 | No valid frame pointer |
| EFLAGS.IF | 0 | Interrupts disabled (CLI) |
| EFLAGS.DF | 0 | Direction flag cleared (CLD) |
| CR0.PE | 1 | Protected mode enabled |
| CR0.PG | 0 | Paging OFF — all addresses are physical |
| A20 | Enabled | Full 32-bit address space accessible |
| FPU | Uninitialized | Kernel must call fpu_init() before any FP use |
| `[ESP+4]` | 0x10000 | Pointer to boot_info (cdecl calling convention) |

**kernel_main must not return.** There is no valid return address on the stack. The bootloader does not set one. If kernel_main returns, behavior is undefined (likely triple fault). The kernel should halt or enter an infinite loop on fatal error.

### boot_info Struct

**Located at physical address 0x10000. Size: max 4KB (one page).**

```c
/* Shared between bootloader (ASM) and kernel (C) */

#define BOOT_MAGIC          0x434C4F53  /* 'CLOS' */
#define MAX_E820_ENTRIES     32
#define MAX_VBE_MODES        32
#define MAX_KERNEL_SEGMENTS  8

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
    uint32_t kernel_phys_start;      /* Always 0x100000 by linker contract.
                                      * Bootloader asserts lowest p_paddr == this. */
    uint32_t kernel_phys_end;        /* highest (p_paddr + p_memsz), page-aligned */
    uint32_t kernel_loaded_bytes;    /* sum of p_filesz across all PT_LOAD segments
                                      * (bytes actually read from disk, excluding
                                      * BSS zero-fill) */
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

/* Static assert in kernel to verify (compiler checks at build time): */
_Static_assert(sizeof(struct boot_info) <= 4096, "boot_info exceeds 4KB page!");
```

### Linker Script Contract

The kernel linker script (linker.ld) MUST follow these rules:

```ld
/* linker.ld — AIOS v2 kernel */
ENTRY(kernel_main)

SECTIONS
{
    /* HARD CONTRACT: Kernel loads at physical 1MB.
     * p_vaddr = p_paddr = addresses in this script.
     * The bootloader loads to p_paddr. Paging is OFF when kernel starts.
     * All addresses here ARE physical addresses. */
    . = 0x100000;

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;

    /* Discard unneeded sections */
    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
```

### Phase 0 Acceptance Tests

1. **Stage 1 loads Stage 2:** Serial output shows Stage 2 entry message.
2. **Stage 1 retry logic:** With a test harness that simulates a first-read failure, Stage 1 retries and succeeds on subsequent attempt.
3. **E820 works:** boot_info.e820_count > 0, max_phys_addr >= configured RAM size.
4. **E820 fallback:** With E820 disabled (if testable), E801 fallback produces a valid synthetic memory map.
5. **VBE mode set:** Screen switches to graphical mode (visible), boot_info.fb_addr != 0.
6. **VBE fallback:** With VBE unavailable, boot_info.fb_addr == 0 and kernel enters text mode gracefully.
7. **ELF loads:** Kernel entry point executes via unreal mode ELF streaming. Serial output shows "Unreal mode active" followed by "Kernel loaded", then first serial output from kernel_main appears.
8. **ELF validation:** Boot halts with error if given a corrupt/truncated ELF.
9. **ELF streaming:** A kernel larger than 512KB loads correctly, proving segments are streamed to p_paddr and not limited by the scratch buffer.
10. **boot_info integrity:** Kernel validates magic == BOOT_MAGIC. All fields are sane. Static assert on struct size passes.
11. **Kernel segments:** boot_info.kernel_segment_count > 0, all segments have phys_start < phys_end, all within 0x100000 - 0x1000000 range.
12. **Entry ABI:** Kernel verifies on entry: interrupts disabled (read EFLAGS), paging off (read CR0), CS/DS correct, boot_info pointer valid at [ESP+4].
13. **Build-time checks:** Stage 2 size check in Makefile passes. `_Static_assert` on boot_info size passes compilation.

---

## Phase 1: Memory Management v2

### Overview

Three subsystems: Physical Memory Manager (PMM), Virtual Memory Manager (VMM), and Kernel Heap. All designed for the final system from day one.

### 1.1 — Physical Memory Manager (kernel/pmm.c)

Bitmap-based page allocator. Each bit represents one 4KB page (0 = free, 1 = used).

**Key difference from v1:** Dynamic bitmap sized to actual RAM, supports full detected memory, per-segment kernel reservation.

#### Design

```c
#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define PAGES_PER_BYTE  8

/* PMM state */
static uint8_t* bitmap;           /* dynamically placed, NOT a fixed BSS array */
static uint32_t bitmap_size;      /* bytes in bitmap */
static uint32_t total_pages;      /* total manageable pages */
static uint32_t used_pages;
static uint32_t max_phys_addr;    /* highest physical address (from boot_info) */
static uint32_t next_alloc_hint;  /* byte index for next-fit search */
```

#### Initialization (pmm_init)

1. Receive pointer to boot_info
2. Compute total_pages from boot_info.max_phys_addr (capped at whatever fits in 32-bit address space)
3. Compute bitmap_size = (total_pages + 7) / 8
4. **Place bitmap:** scan E820 usable regions for a free area large enough for the bitmap that does NOT overlap:
   - Any kernel segment (from boot_info.kernel_segments[])
   - The boot_info page (0x10000)
   - Low memory below 0x100000
   - The bitmap's own location (trivially satisfied)
5. Mark ALL pages as used (0xFF fill) — safe default
6. Walk E820 map, mark usable pages as free
7. **Re-reserve:** for each boot_info.kernel_segments[i], mark those pages as used. Also reserve: page at 0x10000 (boot_info), low memory 0x00000-0xFFFFF, and the bitmap's own pages.
8. Set next_alloc_hint = 0

#### Allocation (pmm_alloc_page / pmm_alloc_pages)

```c
/* Single page allocation — O(1) amortized with next-fit */
uint32_t pmm_alloc_page(void);

/* Contiguous multi-page allocation — for DMA buffers, framebuffer back buffer */
/* Scans for 'count' consecutive free pages. Returns physical address or 0. */
uint32_t pmm_alloc_pages(uint32_t count);

/* Free */
void pmm_free_page(uint32_t phys_addr);
void pmm_free_pages(uint32_t phys_addr, uint32_t count);
```

Single-page alloc uses next-fit: start scanning from next_alloc_hint, wrap around if needed. When found, update hint to current position.

Multi-page alloc does a linear scan for the required consecutive free bits. This is O(n) but only used for large allocations (framebuffer back buffer, DMA rings) which happen rarely.

#### API

```c
void     pmm_init(struct boot_info* info);
uint32_t pmm_alloc_page(void);
uint32_t pmm_alloc_pages(uint32_t count);
void     pmm_free_page(uint32_t phys_addr);
void     pmm_free_pages(uint32_t phys_addr, uint32_t count);
uint32_t pmm_get_total_pages(void);
uint32_t pmm_get_free_pages(void);
uint32_t pmm_get_used_pages(void);
uint32_t pmm_get_max_phys_addr(void);  /* bytes — highest physical address */
```

### 1.2 — Virtual Memory Manager (kernel/vmm.c)

Sets up x86 paging with 4KB page tables. Identity-maps required regions.

#### Design Decision: Selective Identity Mapping

**v1 mapped the entire 4GB address space.** That was simple but wasteful and semantically sloppy — it mapped MMIO holes, reserved regions, and non-existent memory as present.

**v2 maps only what the kernel actually needs:**

- Kernel image (all segments from boot_info)
- Low memory 0x00000-0xFFFFF (IVT, BIOS data, VGA, boot_info at 0x10000)
- PMM bitmap
- VMM page tables themselves
- Framebuffer (with NOCACHE | WRITETHROUGH flags)
- Kernel heap region (grows as heap grows)
- E1000 MMIO (mapped on demand when NIC driver initializes)

**Intentional design tradeoff:** This is still identity mapping (virt == phys) with no higher-half kernel or user/kernel split. For a single-address-space hobby OS with no process isolation, this is the correct choice. It eliminates an entire class of address translation bugs while keeping the mapping bounded to what actually exists and is needed.

All pages use 4KB page tables from the start — no 4MB PSE pages, no runtime splitting.

#### Implementation

```c
/* Page directory: 1024 entries, each points to a page table */
/* Page tables: allocated from PMM as needed */
/* Both must be page-aligned (4KB) */

static uint32_t* page_directory;  /* allocated from PMM */

/* Page table entry flags */
#define PTE_PRESENT      0x001
#define PTE_WRITABLE     0x002
#define PTE_USER         0x004
#define PTE_WRITETHROUGH 0x008
#define PTE_NOCACHE      0x010
#define PTE_ACCESSED     0x020
#define PTE_DIRTY        0x040

void vmm_init(struct boot_info* info);

/* Map a single 4KB page */
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

/* Map a contiguous range of pages */
void vmm_map_range(uint32_t virt_start, uint32_t phys_start,
                   uint32_t size, uint32_t flags);

/* Unmap a page */
void vmm_unmap_page(uint32_t virt);

/* Change flags on an already-mapped page */
void vmm_set_flags(uint32_t virt, uint32_t flags);

/* Flush TLB for a specific address */
void vmm_flush_tlb(uint32_t virt);

/* Flush entire TLB (reload CR3) */
void vmm_flush_tlb_all(void);
```

#### vmm_init sequence:

1. Allocate page directory from PMM (one page, zeroed)
2. Map low memory: 0x00000 - 0xFFFFF with PRESENT | WRITABLE (256 pages). Mark VGA region 0xA0000-0xBFFFF with NOCACHE | WRITETHROUGH additionally.
3. Map kernel segments: for each boot_info.kernel_segments[i], map phys_start to phys_end with PRESENT | WRITABLE
4. Map boot_info page: 0x10000 with PRESENT (read-only is fine)
5. Map PMM bitmap: wherever it was placed, with PRESENT | WRITABLE
6. Map framebuffer: boot_info.fb_addr for (fb_pitch x fb_height) bytes with PRESENT | WRITABLE | NOCACHE | WRITETHROUGH
7. Map initial heap region (see heap section)
8. Map page directory and page tables themselves (so we can modify them after paging is on)
9. Enable paging: load CR3, set CR0.PG
10. **Do NOT enable PSE (CR4 bit 4).** We don't use 4MB pages.

#### Mapping new regions after init:

When a subsystem needs MMIO access (e.g., e1000 NIC), it calls `vmm_map_range()` to add identity mappings for those physical addresses. This is clean and explicit — no "everything is already mapped" surprises.

### 1.3 — Kernel Heap (kernel/heap.c)

Dual-allocator design: **slab allocator** for small allocations (<= 2048 bytes), **buddy allocator** for large allocations (> 2048 bytes).

#### Why two allocators

- **Slab:** O(1) alloc and free for fixed-size blocks. Zero internal fragmentation within a size class. Minimal overhead per allocation. Perfect for Lua's thousands of small allocs/sec.
- **Buddy:** O(log n) alloc and free for variable-size large blocks. Power-of-2 splitting and merging prevents external fragmentation. Good for buffers, textures, large data structures.

#### Page Ownership Table

**Fixes the v1 bug where kfree had to guess which allocator owned a pointer.**

```c
/* Every physical page used by the heap has an entry here */
typedef enum {
    PAGE_UNUSED = 0,
    PAGE_SLAB,      /* owned by slab allocator — kfree routes to slab_free */
    PAGE_BUDDY,     /* owned by buddy allocator — kfree routes to buddy_free */
    PAGE_RESERVED   /* NOT routable by kfree. Catch-all for pages managed by
                     * other subsystems: PMM bitmap, page tables, framebuffer
                     * back buffer, DMA buffers, etc. Calling kfree on a
                     * PAGE_RESERVED page is a bug and will be caught by assert. */
} page_owner_t;

/* One byte per page. For 128MB RAM = 32768 entries = 32KB.
 * Allocated from PMM during heap init. */
static uint8_t* page_ownership;
```

**Scope contract:** The page_ownership table is a routing mechanism for `kfree`, not a general-purpose page provenance tracker. It classifies pages into exactly three actionable categories:
- **PAGE_SLAB / PAGE_BUDDY:** heap-managed pages. `kfree` uses this to dispatch correctly.
- **PAGE_RESERVED:** everything else. These pages are managed by their respective subsystems (PMM, VMM, framebuffer driver, etc.) and must never be passed to `kfree`.

If a future subsystem allocates pages from PMM for its own use, it should mark them PAGE_RESERVED via `heap_mark_reserved(phys_addr, count)`.

When `kfree(ptr)` is called:
1. Compute page index: `page = (uint32_t)ptr >> PAGE_SHIFT`
2. Look up `page_ownership[page]`
3. Route to `slab_free()` or `buddy_free()` accordingly
4. **Assert on PAGE_RESERVED or PAGE_UNUSED — these are bugs in the caller.**
5. **No magic number sniffing. No ambiguity. O(1) lookup.**

#### Slab Allocator

**Size classes:** 16, 32, 64, 128, 256, 512, 1024, 2048 bytes (8 classes)

Each size class has a list of "slab pages." Each slab page is one 4KB physical page containing:
- A small header at the start (slab metadata)
- Fixed-size blocks filling the rest of the page

```c
struct slab_header {
    uint16_t block_size;      /* size class this slab serves */
    uint16_t total_blocks;    /* blocks in this slab */
    uint16_t free_count;      /* blocks currently free */
    uint16_t first_free;      /* index of first free block (free list head) */
    struct slab_header* next; /* next slab page in this size class */
};

/* Each free block's first 2 bytes store the index of the next free block.
 * This is a free list embedded within the blocks themselves.
 * When a block is allocated, these bytes become part of the user's data. */

#define SLAB_HEADER_SIZE  sizeof(struct slab_header)  /* ~12 bytes */

/* For a 32-byte size class in a 4KB page:
 *   header = 12 bytes (padded to 32 for alignment)
 *   blocks = (4096 - 32) / 32 = 127 blocks per page
 *   alloc = pop first_free (O(1))
 *   free  = push to first_free (O(1))
 */
```

**Slab allocation:**
1. Determine size class: round up requested size to nearest class
2. Find the slab list for that class
3. Find a slab with free_count > 0 (check head first — usually hits)
4. Pop from the free list: read next-free index from block, update first_free
5. Decrement free_count, return pointer
6. If no slab has free blocks, allocate a new page from PMM, initialize as slab, add to list

**Slab free:**
1. Compute which slab page this pointer belongs to: `slab_page = ptr & ~0xFFF`
2. Read slab_header from page start
3. Compute block index from pointer offset
4. Push onto free list: store current first_free in block, set first_free = this index
5. Increment free_count
6. If free_count == total_blocks, optionally return the page to PMM (shrink)

#### Buddy Allocator

Manages allocation of large blocks (> 2048 bytes) in power-of-2 sizes.

**Minimum block: 4096 bytes (1 page). Maximum block: defined by heap size.**

```c
/* Buddy allocator manages a contiguous region of physical pages.
 * Heap init allocates a large chunk from PMM (e.g., 16MB) and
 * hands it to the buddy allocator. */

#define BUDDY_MIN_ORDER  12  /* 2^12 = 4KB minimum block */
#define BUDDY_MAX_ORDER  24  /* 2^24 = 16MB maximum block */
#define BUDDY_LEVELS     (BUDDY_MAX_ORDER - BUDDY_MIN_ORDER + 1)  /* 13 levels */
```

**Block memory layout:**

Every buddy block (free or allocated) begins with an 8-byte header:

```c
struct buddy_header {
    uint32_t magic;     /* BUDDY_MAGIC for debug validation */
    uint8_t  order;
    uint8_t  is_free;
    uint16_t reserved;
};

#define BUDDY_MAGIC     0x42554459  /* 'BUDY' */
#define BUDDY_HDR_SIZE  8           /* padded to 8 bytes */
```

The BUDDY_MAGIC is only used for debug validation (assert on free), NOT for allocator routing. Routing uses the page_ownership table.

**Free block overlay:** When a block is free, the space immediately after the header is repurposed as an intrusive doubly-linked list node for the free list:

```c
/* This struct is overlaid on the user-data area of FREE blocks only.
 * It occupies bytes [8..23] of the block (after the buddy_header).
 * When the block is allocated, these bytes are available to the caller. */
struct buddy_free_node {
    struct buddy_free_node* next;
    struct buddy_free_node* prev;
};

/* Free list head for each order level */
static struct buddy_free_node* free_lists[BUDDY_LEVELS];
```

**Layout summary:**
```
Allocated block:  [buddy_header (8 bytes)] [user data ...............]
Free block:       [buddy_header (8 bytes)] [buddy_free_node (8 bytes)] [unused ...]
```

The minimum block size is 4KB, so there is always room for both the header and the free-list node. User-visible allocation size is `2^order - BUDDY_HDR_SIZE`.

**Buddy allocation:**
1. Round requested size up to next power of 2 (minimum 4096), accounting for BUDDY_HDR_SIZE overhead
2. Determine order = log2(size)
3. Find the smallest free block at that order or above
4. If found at the exact order, remove from free list, mark as not free, return pointer to user data (header + 8)
5. If found at a higher order, split in half repeatedly until at the right order. The "buddy" half gets its own header and goes onto the free list for its order.

**Buddy free:**
1. Read the block's header (8 bytes before the user pointer) to get order
2. Assert magic == BUDDY_MAGIC (debug check)
3. Check if its buddy (the other half of the pair) is free at the same order
4. If yes, merge: remove buddy from its free list, combine into a block one order higher, repeat
5. If no, mark as free, add free_node overlay, insert into the free list at its order

#### Aligned Allocation (kmalloc_aligned / kfree_aligned)

For allocations requiring alignment greater than the natural alignment of the allocators (slab blocks are naturally aligned to their size class; buddy blocks are naturally page-aligned).

**Primary use case:** FPU/SSE state buffers (512 bytes, 16-byte aligned), DMA buffers (page-aligned or higher).

**Implementation contract:**

```c
void* kmalloc_aligned(size_t size, size_t alignment);
void  kfree_aligned(void* ptr);
```

**Algorithm:**
1. If `alignment <= 8`, delegate to `kmalloc(size)` — slab blocks of size >= 16 are naturally 8-byte aligned (slab header is padded to the block size). In this case, `kfree_aligned` is equivalent to `kfree`.
2. If `alignment > PAGE_SIZE (4096)`, the allocation is forced through the buddy allocator regardless of size, since only buddy provides page-granular placement.
3. Otherwise, over-allocate: `kmalloc(size + alignment + sizeof(void*))`.
4. Compute the aligned address within the over-allocated region, ensuring at least `sizeof(void*)` bytes before it.
5. Store the original `kmalloc` pointer in the `sizeof(void*)` bytes immediately before the aligned address.
6. Return the aligned address.

**kfree_aligned:**
1. Read the original pointer from `((void**)ptr)[-1]`.
2. Call `kfree(original_ptr)`.

**Restrictions:**
- `kmalloc_usable_size()` must NOT be called on aligned pointers — the result would be meaningless (it would report the size of the underlying over-allocation).
- `krealloc()` must NOT be called on aligned pointers — the stored original pointer would be invalidated. To resize an aligned allocation, allocate new, copy, free old.
- These restrictions are documented in the API and enforced by assert where possible (e.g., checking alignment metadata consistency).

#### Public Heap API

```c
/* Initialize heap — call after PMM and VMM are up */
void heap_init(struct boot_info* info);

/* Allocate memory. Routes to slab (<=2048) or buddy (>2048). */
void* kmalloc(size_t size);

/* Allocate zeroed memory */
void* kzmalloc(size_t size);

/* Allocate aligned memory (see Aligned Allocation section for contract) */
void* kmalloc_aligned(size_t size, size_t alignment);

/* Reallocate. Handles cross-allocator moves (slab->buddy or buddy->buddy).
 * Must NOT be called on pointers from kmalloc_aligned. */
void* krealloc(void* ptr, size_t new_size);

/* Free memory. Uses page_ownership table to route correctly.
 * Must NOT be called on pointers from kmalloc_aligned — use kfree_aligned. */
void kfree(void* ptr);

/* Free aligned memory (see Aligned Allocation section for contract) */
void kfree_aligned(void* ptr);

/* Get usable size of an allocation.
 * Slab: returns the size class (e.g., 64 for a 50-byte alloc).
 * Buddy: returns (2^order - BUDDY_HDR_SIZE).
 * Must NOT be called on pointers from kmalloc_aligned. */
size_t kmalloc_usable_size(void* ptr);

/* Mark pages as PAGE_RESERVED in page_ownership table.
 * Called by subsystems that allocate pages from PMM for their own use. */
void heap_mark_reserved(uint32_t phys_addr, uint32_t page_count);

/* Stats */
uint32_t heap_get_used(void);
uint32_t heap_get_free(void);
uint32_t heap_get_slab_used(void);
uint32_t heap_get_buddy_used(void);
```

**krealloc logic:**
1. If ptr is NULL, equivalent to kmalloc(new_size)
2. Get old_size via kmalloc_usable_size(ptr)
3. If new_size <= old_size and same allocator can serve it, return ptr (no-op)
4. kmalloc(new_size), memcpy min(old_size, new_size), kfree(ptr), return new

#### Heap Init Sequence

1. Allocate page_ownership table from PMM: one byte per page, zero-filled
2. Allocate buddy region from PMM: pmm_alloc_pages(N) for a contiguous block (e.g., 4096 pages = 16MB)
3. Mark those pages as PAGE_BUDDY in page_ownership
4. Initialize buddy free lists: the entire region starts as one large free block
5. Slab allocator starts with no pages — allocates from PMM on demand
6. Map the heap region in VMM if not already covered

### Phase 1 Acceptance Tests

1. **PMM basic:** alloc 1000 pages, free them all, verify free count returns to original. Alloc again — should succeed, proving pages were actually freed.
2. **PMM stress:** alloc pages until out of memory. Verify used_pages == total usable pages. Free all. Verify free count matches.
3. **PMM contiguous:** alloc_pages(256) succeeds (1MB contiguous). Free it. Alloc_pages(256) succeeds again (same or different location, doesn't matter).
4. **VMM mapping:** map a new page, write to it, read back — matches. Unmap it.
5. **VMM flags:** map a page with NOCACHE, verify the page table entry has NOCACHE bit set (read PTE directly).
6. **Slab basic:** alloc and free 10,000 x 32-byte blocks. No memory leak (slab free count returns to original). No corruption.
7. **Slab stress:** alloc 10,000 blocks of random sizes (16-2048), free them in random order. Heap stats show all memory recovered.
8. **Buddy basic:** alloc and free a 64KB block. Alloc again — succeeds.
9. **Buddy splitting:** alloc 4KB, then 4KB, then 4KB. Free the middle one. Alloc 4KB — succeeds (reuses freed block). Free all — buddy merges back to original large block.
10. **kfree routing:** alloc via slab (64 bytes), free it. Alloc via buddy (8KB), free it. No crash, no corruption, page_ownership correctly identifies both.
11. **kfree on reserved page:** allocate a page from PMM, mark PAGE_RESERVED via heap_mark_reserved, attempt kfree — assert fires (caught, not a crash in release).
12. **krealloc:** alloc 32 bytes, write data, krealloc to 128 bytes, verify data preserved. krealloc to 8KB (crosses slab->buddy boundary), verify data preserved.
13. **kmalloc_aligned:** alloc 512 bytes with 16-byte alignment, verify address is 16-byte aligned. Write pattern, free via kfree_aligned, verify no leak. Alloc with PAGE_SIZE alignment, verify page-aligned.
14. **Page ownership:** after all tests, verify page_ownership table is consistent with PMM state.
15. **Memory accounting:** heap_get_used() + heap_get_free() == total heap size at all times.

---

## Phase 2: Multitasking v2

### Overview

Preemptive multitasking with full interrupt infrastructure: GDT with TSS, IDT with 256 entries, CPU exception handlers, hardware IRQ handlers with PIC remapping, PIT timer at 250Hz, FPU/SSE save/restore, and a priority scheduler with context switching and starvation prevention.

### Init Order (strict)

```
gdt_init()          → Install kernel GDT with TSS entry
idt_init()          → Install 256-entry IDT (empty, loaded with lidt)
isr_init()          → Wire exception handlers 0-31 into IDT
irq_init()          → Remap PIC (IRQ0-7→INT32-39, IRQ8-15→INT40-47), wire IRQ stubs
fpu_init()          → CR0.EM=0, CR0.MP=1, CR4.OSFXSR=1, CR4.OSXMMEXCPT=1, fninit
timer_init()        → PIT ch0 mode 2, divisor=4773 (250Hz), register IRQ0, unmask IRQ0
scheduler_init()    → Set up task 0 (kernel), create idle task
sti                 → Enable interrupts (timer starts, preemption begins)
rdtsc_calibrate()   → Count TSC cycles over 100 PIT ticks (400ms)
```

### 2.1 — GDT with TSS (kernel/gdt.c)

The bootloader's GDT has only null + code + data segments. The kernel installs its own GDT with a TSS entry for hardware task state.

```c
/* 4 GDT entries */
[0] null descriptor
[1] kernel code: selector 0x08, access=0x9A, gran=0xCF (base 0, limit 4GB, exec/read, 32-bit)
[2] kernel data: selector 0x10, access=0x92, gran=0xCF (base 0, limit 4GB, read/write, 32-bit)
[3] TSS:         selector 0x18, access=0x89, gran=0x00 (32-bit TSS, not busy)
```

The TSS only uses `esp0` and `ss0=0x10`. `gdt_set_kernel_stack(esp0)` updates `tss.esp0` on every context switch so hardware interrupts during a task use the correct kernel stack top.

After `lgdt`, a far jump to `0x08:` reloads CS, and segment registers are reloaded with `0x10`. `ltr 0x18` loads the TSS.

### 2.2 — IDT (kernel/idt.c)

256-entry IDT, all zeroed initially. `idt_set_gate(num, handler, selector=0x08, type=0x8E)` wires individual entries. ISR and IRQ init functions populate it. Loaded via `lidt`.

### 2.3 — CPU Exception Handlers (kernel/isr_stubs.asm + kernel/isr.c)

NASM macros generate 32 exception stubs:
- `ISR_NOERRCODE n`: push 0 (dummy error code), push n, jmp common
- `ISR_ERRCODE n`: push n (CPU already pushed error code), jmp common
- Error-code exceptions: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30

Common stub: pusha, push segment regs, load kernel segments (0x10), push esp, call `isr_common_handler`, restore segments, popa, add esp 8, iret.

`isr_common_handler(struct registers* regs)` prints exception info + registers to serial, reads CR2 for page faults (#14), then calls `kernel_panic`.

```c
struct registers {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;  /* pusha order */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;  /* CPU-pushed */
};
```

### 2.4 — Hardware IRQ Handlers (kernel/irq_stubs.asm + kernel/irq.c)

**PIC remapping:** ICW1-4 sequence remaps IRQ 0-7 → INT 32-39, IRQ 8-15 → INT 40-47. All IRQs masked initially; individual drivers unmask as needed.

16 IRQ stubs (same structure as ISR stubs) call `irq_common_handler(struct registers* regs)`.

**CRITICAL: EOI is sent BEFORE calling the handler.** If the handler calls `schedule()→task_switch()`, the EOI would be deferred until the old task resumes, freezing the timer. Sending EOI first is safe because IF=0 in the interrupt gate (no re-entrance).

```c
void irq_common_handler(struct registers* regs) {
    int irq_num = regs->int_no - 32;
    if (irq_num >= 8) outb(0xA0, 0x20);  /* slave EOI */
    outb(0x20, 0x20);                      /* master EOI */
    if (irq_handlers[irq_num]) irq_handlers[irq_num]();
}
```

### 2.5 — FPU/SSE Init (kernel/fpu.c)

```c
void fpu_init(void) {
    /* CR0: clear EM (bit 2), set MP (bit 1) */
    /* CR4: set OSFXSR (bit 9), OSXMMEXCPT (bit 10) */
    /* fninit to initialize FPU state */
}
```

Enables fxsave/fxrstor for the scheduler. The kernel itself is compiled with `-mno-sse -mno-mmx -mno-sse2` to prevent compiler-emitted SSE in interrupt handlers. The renderer uses `RENDERER_CFLAGS` with SSE2 enabled.

### 2.6 — PIT Timer (drivers/timer.c)

```c
#define PIT_FREQUENCY    250   /* Hz — design contract */
#define PIT_BASE_FREQ    1193182

init_result_t timer_init(void);
void     timer_handler(void);       /* ticks++, schedule() — called from IRQ0 */
uint64_t timer_get_ticks(void);     /* 64-bit, interrupt-safe read via pushf/cli/popf */
uint32_t timer_get_uptime_seconds(void);
uint32_t timer_get_frequency(void); /* returns PIT_FREQUENCY */
void     timer_wait(uint32_t ms);   /* hlt loop until target ticks reached */
```

PIT channel 0, mode 2 (rate generator), divisor = 1193182/250 = 4773.

**64-bit tick counter** eliminates the 32-bit wrap problem (wraps in ~2.3 billion years at 250Hz). Interrupt-safe read uses `pushf/cli/read/popf` (not bare `cli/sti`, which is unsafe from interrupt context).

### 2.7 — RDTSC High-Resolution Timer (kernel/rdtsc.c)

```c
static inline uint64_t rdtsc(void);    /* inline rdtsc instruction */
init_result_t rdtsc_calibrate(void);   /* measure TSC over 100 PIT ticks (400ms) */
uint64_t rdtsc_get_frequency(void);    /* cycles per second */
uint64_t rdtsc_to_us(uint64_t cycles); /* convert to microseconds */
```

Calibration runs after `sti` (needs timer interrupts). Waits for a tick boundary, then counts TSC cycles over 100 ticks.

### 2.8 — Task Structure

```c
#define MAX_TASKS       32
#define TASK_STACK_SIZE 16384    /* 16KB per task */

typedef enum {
    PRIORITY_HIGH   = 0,
    PRIORITY_NORMAL = 1,
    PRIORITY_LOW    = 2,
    PRIORITY_IDLE   = 3
} task_priority_t;

typedef enum {
    TASK_UNUSED  = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,
    TASK_EXITED
} task_state_t;

struct task {
    uint32_t esp;
    int      id;
    const char* name;
    task_state_t state;
    task_priority_t priority;
    uint32_t stack_base;
    uint32_t stack_size;
    uint8_t* fpu_state;              /* 512 bytes via kmalloc_aligned(512, 16) */
    bool     fpu_initialized;
    uint64_t sleep_until;            /* 64-bit tick count — no wrap issues */
    uint32_t cpu_ticks;
    uint32_t total_ticks;
    bool     needs_cleanup;
};
```

### 2.9 — Scheduler (kernel/scheduler.c + kernel/scheduler_asm.asm)

#### Context Switch Assembly

```nasm
; void task_switch(uint32_t* old_esp, uint32_t new_esp)
task_switch:
    push ebp / ebx / esi / edi
    mov eax, [esp+20]     ; &old_esp
    mov [eax], esp         ; save current ESP
    mov esp, [esp+24]      ; load new ESP (from OLD stack before switch)
    pop edi / esi / ebx / ebp
    ret                    ; returns to new task's saved EIP
```

#### New Task Stack Setup

New tasks go through a trampoline that enables interrupts before the entry function runs. This is necessary because `schedule()` disables interrupts (`cli`) before `task_switch`, so new tasks would otherwise start with IF=0.

```
[top of 16KB stack, 16-byte aligned]
task_exit_wrapper       ← return addr when entry function returns
entry_function          ← trampoline's ret pops this
task_entry_trampoline   ← task_switch's ret jumps here (does sti, then ret)
0                       ← saved EBP
0                       ← saved EBX
0                       ← saved ESI
0                       ← saved EDI
[ESP points here]
```

#### Idle Task

```c
static void idle_task_entry(void) {
    while (1) {
        __asm__ __volatile__("sti; hlt");  /* sti before hlt — ensures wakeup */
    }
}
```

The `sti; hlt` pair is critical: without `sti`, `hlt` would halt the CPU permanently since no interrupts could wake it.

#### schedule() — Called from Timer IRQ and task_yield

```c
__attribute__((noinline))
void schedule(void) {
    if (!scheduler_enabled) return;

    /* Disable interrupts to prevent re-entrant schedule() calls.
     * Timer IRQ can fire while in schedule() via task_yield(). */
    pushf/cli (save flags)

    1. Wake sleeping tasks (64-bit comparison)
    2. Cleanup exited tasks (deferred: kfree stack, kfree_aligned FPU)
    3. CPU accounting (idle_ticks / window for cpu_usage_pct)
    4. Starvation prevention: boost starved NORMAL/LOW to HIGH for one slice
       (threshold: >=250 ticks without running; original priority tracked separately)
    5. find_next_task(): priority scan HIGH→IDLE, round-robin within priority
    6. Restore all boosted priorities after selection
    7. FPU save (fxsave) old task, FPU restore (fxrstor) or fninit for new task
    8. Update TSS esp0 via gdt_set_kernel_stack()
    9. task_switch(&old.esp, new.esp)
    10. [compiler barrier — prevents tail-call optimization of task_switch]
    11. Restore saved flags (popf — re-enables interrupts for resumed task)
}
```

**Re-entrancy protection:** `schedule()` is called both from the timer IRQ (IF=0 already) and from `task_yield()` (IF=1). Without the `pushf/cli` guard, a timer IRQ during a yield-triggered `schedule()` would re-enter and corrupt shared state.

#### CPU Usage

```c
#define ACCOUNTING_WINDOW 250  /* 1 second at 250 Hz */

/* In schedule(): */
window_ticks++;
if (current_task == idle_task_id) idle_ticks++;
if (window_ticks >= ACCOUNTING_WINDOW) {
    last_window_idle = idle_ticks;
    last_window_total = window_ticks;
    window_ticks = idle_ticks = 0;
}

/* scheduler_get_cpu_usage(): */
/* Returns (window - idle) * 100 / window from last completed window */
```

#### Task Lifecycle Contract

1. **task_create()** → TASK_READY. Allocates stack (kmalloc) + FPU buffer (kmalloc_aligned).
2. **schedule() picks task** → TASK_RUNNING. Only one task is RUNNING at any time.
3. **schedule() preempts** → back to TASK_READY.
4. **task_sleep(ms)** → TASK_SLEEPING, sleep_until set, calls task_yield().
5. **task_yield()** → `sti; schedule()`. Ensures interrupts enabled after return.
6. **task_exit()** → TASK_EXITED, needs_cleanup=true. Infinite yield loop. Scheduler frees resources on next tick.
7. **task_kill(id)** → sets TASK_EXITED + needs_cleanup. Protected: kernel (task 0) and idle cannot be killed.

#### Task API

```c
init_result_t scheduler_init(void);
void schedule(void);
int  task_create(const char* name, void (*entry)(void), task_priority_t priority);
void task_sleep(uint32_t ms);
void task_yield(void);
void task_exit(void);
int  task_kill(int id);
struct task* task_get_current(void);
struct task* task_get(int index);
int  task_get_count(void);
int  scheduler_get_cpu_usage(void);
```

### Phase 2 Acceptance Tests

1. **Basic switching:** 3 tasks increment counters. After 1s, all > 0.
2. **Priority:** HIGH vs LOW task. HIGH gets >5x more ticks in 1s.
3. **Sleep accuracy:** task_sleep(500). Verify ~125 ticks elapsed (±5 ticks).
4. **Sleep wrap-safe:** 64-bit trivially satisfied. Verify sleep(100ms) works.
5. **Task exit cleanup:** Create 100 tasks (in batches of 20) that exit. Heap returns to baseline, task_count returns.
6. **FPU isolation:** Task A loads 1.0, yields, checks 1.0. Task B loads 2.0, yields, checks 2.0. 100 iterations each.
7. **FPU stress:** 5 tasks doing different FP math for 5 seconds. Results correct.
8. **Idle CPU:** Only kernel+idle running. scheduler_get_cpu_usage() ≈ 0-5%.
9. **CPU measurement:** Tight-loop task. scheduler_get_cpu_usage() ≈ 90-100%.
10. **Kill task:** Create task, kill it, verify cleanup. Verify kill(idle)=-1, kill(0)=-1.
11. **Starvation prevention:** HIGH tight-loop + NORMAL counter. After 5s, NORMAL counter > 0.
12. **Yield with interrupts:** cli, task_yield(), verify IF=1 after return.

---

## Phase 3: Drivers v2

### Overview

Drivers for all hardware needed by the OS. Each driver is a self-contained module with a clean init/API boundary. Init functions return proper status codes. Drivers that need MMIO call vmm_map_range() explicitly rather than relying on pre-mapped address space.

### 3.1 — Timer (drivers/timer.c)

```c
#define PIT_FREQUENCY 250   /* Hz — design contract */

/* PIT base oscillator */
#define PIT_BASE_FREQ 1193182

init_result_t timer_init(void);
void     timer_handler(void);          /* called from IRQ0 */
uint64_t timer_get_ticks(void);        /* 64-bit, no wrap */
uint32_t timer_get_uptime_seconds(void);
uint32_t timer_get_frequency(void);    /* returns PIT_FREQUENCY */
void     timer_wait(uint32_t ms);      /* busy wait using hlt */
```

**Implementation:**
- PIT channel 0, mode 2 (rate generator), divisor = PIT_BASE_FREQ / PIT_FREQUENCY
- 64-bit tick counter, interrupt-safe read (cli/sti around 64-bit load)
- timer_wait uses hlt between tick checks (power-efficient)

**Acceptance:** verify tick counter increments at 250 Hz (+/-2%). RDTSC calibration produces a reasonable frequency. Cross-check timer_wait(1000) against RDTSC measurement to verify ~1 second elapsed.

### 3.2 — VGA Text Mode (drivers/vga.c)

Fallback display for early boot and non-graphical mode.

```c
init_result_t vga_init(void);
void vga_putchar(char c);
void vga_print(const char* str);
void vga_print_color(const char* str, uint8_t color);
void vga_clear(void);
void vga_set_cursor(int x, int y);
int  vga_get_cursor_x(void);
int  vga_get_cursor_y(void);
```

- Writes to 0xB8000 (identity-mapped with NOCACHE by VMM)
- 80x25, scrolling, cursor tracking
- Used during boot log, before framebuffer is available
- **If boot_info.fb_addr == 0 (no VBE), VGA text mode is the primary display for the entire session**

### 3.3 — Framebuffer HAL (drivers/framebuffer.c)

**Hardware Abstraction Layer only.** The framebuffer driver does NOT contain drawing primitives. All rendering (2D and 3D) goes through ChaosGL, which is the sole entity that writes to the back buffer and blits to VRAM. See `documents/ChaosGL-spec.md` for the full graphics API.

Only initialized if boot_info.fb_addr != 0.

```c
typedef struct {
    uint32_t fb_addr;    /* physical address of VBE linear framebuffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint8_t  bpp;        /* bits per pixel — must be 32 */
} fb_info_t;

init_result_t fb_init(struct boot_info* info);

/* Returns true if VBE is initialised and framebuffer is available.
 * Returns false if no VBE (system is in text mode — chaos_gl_init() will fail). */
bool fb_get_info(fb_info_t* out);
```

**fb_init:**
1. Check boot_info.fb_addr != 0. If 0, return INIT_FAIL (not FATAL — system runs in text mode).
2. Store fb_addr, width, height, pitch, bpp from boot_info.
3. Verify bpp == 32 (ChaosGL requires BGRX 32bpp).
4. Framebuffer VRAM is already mapped by VMM with NOCACHE | WRITETHROUGH.

**No back buffer, no drawing primitives, no font, no swap.** ChaosGL owns the back buffer (allocated from PMM during `chaos_gl_init()`), all rendering, and the final blit to VRAM.

**Acceptance:** fb_init returns INIT_OK when VBE is available, INIT_FAIL gracefully when not. fb_get_info returns correct dimensions matching boot_info. If VBE unavailable, system continues in VGA text mode.

### 3.4 — PS/2 Keyboard (drivers/keyboard.c)

Full PS/2 keyboard driver with extended scancode support.

```c
init_result_t keyboard_init(void);
void keyboard_handler(void);       /* called from IRQ1 */

/* Text-mode interface (blocking) */
bool keyboard_has_key(void);
char keyboard_getchar(void);
void keyboard_readline(char* buf, int max_len);

/* Game/GUI interface (non-blocking state polling) */
bool keyboard_is_pressed(uint8_t scancode);

/* Key state array — true = currently held down */
extern volatile bool key_state[256];  /* 256 to handle E0-prefixed codes */
```

**Extended scancode handling (E0 prefix):**

PS/2 scan code set 1 uses E0 as a prefix byte for extended keys. The keyboard sends two bytes: 0xE0 followed by the actual scancode.

```c
static bool e0_prefix = false;  /* state machine for multi-byte sequences */

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);

    /* Handle E0 prefix byte */
    if (scancode == 0xE0) {
        e0_prefix = true;
        return;  /* wait for the actual scancode next interrupt */
    }

    bool is_release = scancode & 0x80;
    uint8_t code = scancode & 0x7F;

    /* If E0-prefixed, remap to 128+ range to avoid collision with normal scancodes */
    if (e0_prefix) {
        code += 128;  /* E0-prefixed scancodes stored at indices 128-255 */
        e0_prefix = false;
    }

    /* Update key state */
    key_state[code] = !is_release;

    /* Push event to input queue */
    if (input_is_gui_mode()) {
        uint16_t key_code = code;
        if (key_state[SC_LCTRL] || key_state[SC_RCTRL]) key_code |= KEY_MOD_CTRL;
        if (key_state[SC_LSHIFT] || key_state[SC_RSHIFT]) key_code |= KEY_MOD_SHIFT;
        if (key_state[SC_LALT] || key_state[SC_RALT]) key_code |= KEY_MOD_ALT;

        input_event_t ev = {
            is_release ? EVENT_KEY_UP : EVENT_KEY_DOWN,
            key_code, 0, 0, 0
        };
        input_push(&ev);
    }

    /* For text mode: translate to ASCII and buffer (only on key DOWN) */
    if (!is_release && !input_is_gui_mode()) {
        char c = translate_to_ascii(code);
        if (c) keyboard_buffer_push(c);
    }
}
```

**Scancode definitions include E0-prefixed keys:**
```c
/* Normal scancodes (0-127) */
#define SC_ESC      0x01
#define SC_W        0x11
#define SC_A        0x1E
#define SC_S        0x1F
#define SC_D        0x20
#define SC_Q        0x10
#define SC_E        0x12
#define SC_SPACE    0x39
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_LCTRL    0x1D
#define SC_LALT     0x38
#define SC_ENTER    0x1C
#define SC_BACKSPACE 0x0E

/* E0-prefixed scancodes (remapped to 128+) */
#define SC_RCTRL    (0x1D + 128)
#define SC_RALT     (0x38 + 128)
#define SC_UP       (0x48 + 128)
#define SC_DOWN     (0x50 + 128)
#define SC_LEFT     (0x4B + 128)
#define SC_RIGHT    (0x4D + 128)
#define SC_HOME     (0x47 + 128)
#define SC_END      (0x4F + 128)
#define SC_PGUP     (0x49 + 128)
#define SC_PGDN     (0x51 + 128)
#define SC_INSERT   (0x52 + 128)
#define SC_DELETE    (0x53 + 128)
```

**Acceptance:** press and release every key, verify key_state toggles correctly. Hold W+A simultaneously, verify both show as pressed. Press right arrow (E0 0x4D), verify SC_RIGHT key state. Press and release shift, verify shift state tracks.

### 3.5 — PS/2 Mouse (drivers/mouse.c)

```c
init_result_t mouse_init(void);
void mouse_handler(void);          /* called from IRQ12 */

/* Desktop mode — absolute position, cursor on screen */
int     mouse_get_x(void);
int     mouse_get_y(void);
uint8_t mouse_get_buttons(void);
void    mouse_set_bounds(int width, int height);

/* Raw mode — for FPS mouselook */
void mouse_set_raw_mode(bool enable);
void mouse_get_delta(int* dx, int* dy);  /* returns accumulated deltas, resets to 0 */
```

**Packet assembly with sync recovery:**
```c
static uint8_t packet[3];
static int packet_idx = 0;

void mouse_handler(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01) || !(status & 0x20)) return;
    uint8_t data = inb(0x60);

    /* Sync check: byte 0 must have bit 3 set */
    if (packet_idx == 0 && !(data & 0x08)) {
        return;  /* out of sync, discard until we see a valid byte 0 */
    }

    packet[packet_idx++] = data;
    if (packet_idx < 3) return;
    packet_idx = 0;

    /* ... process complete packet ... */
}
```

**Raw mode for FPS games:**
```c
static volatile bool raw_mode = false;
static volatile int raw_dx = 0, raw_dy = 0;

void mouse_get_delta(int* dx, int* dy) {
    asm volatile("cli");
    *dx = raw_dx;
    *dy = raw_dy;
    raw_dx = 0;
    raw_dy = 0;
    asm volatile("sti");
}
```

**Acceptance:** move mouse, verify position updates. Click buttons, verify button state. Enable raw mode, verify deltas accumulate and reset correctly.

### 3.6 — Input Event Queue (drivers/input.c)

Shared event queue consumed by GUI/game code, produced by keyboard and mouse IRQ handlers.

```c
#define INPUT_QUEUE_SIZE 256

typedef enum {
    EVENT_NONE = 0,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_MOUSE_MOVE,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_UP,
} event_type_t;

typedef struct {
    uint8_t  type;
    uint16_t key;       /* scancode + modifier flags for keyboard events */
    int16_t  mouse_x;   /* absolute position (desktop) or delta (raw mode) */
    int16_t  mouse_y;
    uint8_t  mouse_btn; /* which button for mouse down/up */
} input_event_t;

/* Ring buffer */
static input_event_t queue[INPUT_QUEUE_SIZE];
static volatile int queue_head = 0;  /* written by IRQ handlers */
static volatile int queue_tail = 0;  /* read by consumer */
static volatile bool queue_overflow = false;
```

**Concurrency contract:**
- **Producers:** keyboard IRQ1 and mouse IRQ12. These are interrupt handlers. On single-core x86, interrupts are serialized by the CPU — the PIC doesn't nest IRQs by default. So only one producer runs at a time. No lock needed between producers.
- **Consumer:** main kernel loop or Lua event poll. Runs in normal (non-interrupt) context.
- **Push (producer):** called with interrupts already disabled (we're in an IRQ handler). Writes to queue[head], advances head. Single-producer-single-consumer ring buffer is naturally safe here.
- **Poll (consumer):** disables interrupts briefly to read queue[tail] and advance tail. This prevents a torn read if an IRQ fires mid-poll.
- **Overflow:** if head catches tail, the newest event is DROPPED and queue_overflow is set to true. Consumer can check and clear this flag.

```c
void input_push(input_event_t* ev) {
    /* Called from IRQ context — interrupts already disabled */
    int next = (queue_head + 1) % INPUT_QUEUE_SIZE;
    if (next == queue_tail) {
        queue_overflow = true;
        return;  /* drop newest event */
    }
    queue[queue_head] = *ev;
    queue_head = next;
}

bool input_poll(input_event_t* ev) {
    asm volatile("cli");
    if (queue_tail == queue_head) {
        asm volatile("sti");
        return false;
    }
    *ev = queue[queue_tail];
    queue_tail = (queue_tail + 1) % INPUT_QUEUE_SIZE;
    asm volatile("sti");
    return true;
}

bool input_has_events(void) {
    return queue_head != queue_tail;
}

bool input_check_overflow(void) {
    bool was = queue_overflow;
    queue_overflow = false;
    return was;
}

/* Mode flag */
static bool gui_mode = false;
void input_set_gui_mode(bool enabled) { gui_mode = enabled; }
bool input_is_gui_mode(void) { return gui_mode; }
```

### 3.7 — ATA/IDE PIO (drivers/ata.c)

Block device driver for disk I/O. Used by ChaosFS.

```c
init_result_t ata_init(void);

/* Block device API (thin abstraction over raw ATA) */
int ata_read_sectors(uint32_t lba, uint32_t count, void* buffer);
int ata_write_sectors(uint32_t lba, uint32_t count, const void* buffer);

/* Device info */
bool     ata_is_present(void);
uint32_t ata_get_sector_count(void);  /* total sectors on disk */
```

- PIO mode, primary bus (ports 0x1F0-0x1F7, control 0x3F6)
- 28-bit LBA addressing (supports up to 128GB)
- **Returns proper error codes.** INIT_OK if drive detected, INIT_FAIL if no drive. Read/write return 0 on success, -1 on error.
- Drives detected via IDENTIFY command during init
- **Retry logic:** read and write operations retry up to 3 times on error, with a soft reset (write 0x04 then 0x00 to control port 0x3F6) between attempts. Real ATA controllers can return transient errors, especially on older hardware or during drive spinup.

**Not a full block device abstraction layer.** For AIOS with one filesystem on one disk, the direct API is correct. If we ever need multiple storage devices, we abstract then. YAGNI.

**Acceptance:** read sector 0, verify MBR signature 0xAA55. Write a test pattern to a high sector, read it back, verify match. Verify retry logic by confirming reads succeed even after a soft reset.

### 3.8 — Serial Debug Output (drivers/serial.c)

```c
init_result_t serial_init(void);
void serial_putchar(char c);
void serial_print(const char* str);
void serial_printf(const char* fmt, ...);  /* basic printf: %d, %x, %s, %u */
```

- COM1 (port 0x3F8), 115200 baud
- Used for debug output visible in QEMU's `-serial stdio` or via a physical serial cable / USB-to-serial adapter on bare metal
- Available from very early boot (before VGA or framebuffer)

### 3.9 — Boot Display (kernel/boot_display.c)

Unified boot logging that uses VGA text mode (or framebuffer if available) with honest status reporting.

```c
void boot_log(const char* component, init_result_t result);
void boot_log_detail(const char* component, init_result_t result, const char* detail);
void boot_print(const char* msg);  /* generic boot message */
```

```c
void boot_log(const char* component, init_result_t result) {
    vga_print("[BOOT] ");
    vga_print(component);
    vga_print("... ");

    switch (result) {
        case INIT_OK:
            vga_print_color("OK", COLOR_GREEN);
            break;
        case INIT_WARN:
            vga_print_color("WARN", COLOR_YELLOW);
            break;
        case INIT_FAIL:
            vga_print_color("FAIL", COLOR_RED);
            break;
        case INIT_FATAL:
            vga_print_color("FATAL", COLOR_LRED);
            vga_print("\n\nBoot halted. Cannot continue.\n");
            serial_print("[BOOT] FATAL: ");
            serial_print(component);
            serial_print("\n");
            while (1) asm volatile("hlt");
    }
    vga_putchar('\n');

    /* Mirror to serial */
    serial_print("[BOOT] ");
    serial_print(component);
    serial_print(result == INIT_OK ? " OK" : result == INIT_WARN ? " WARN" : " FAIL");
    serial_print("\n");
}
```

**With detail string (for printing extra info like memory size, IP address):**
```c
boot_log_detail("Physical memory", pmm_init(boot_info),
    "126 MB free (32286 pages)");
/* Prints: [BOOT] Physical memory... OK  126 MB free (32286 pages) */
```

### Phase 3 Acceptance Tests

1. **Timer:** ticks increment at 250 Hz (+/-2%). RDTSC calibration produces reasonable frequency.
2. **VGA:** text displays correctly, scrolling works, cursor position tracks.
3. **Framebuffer HAL:** if VBE available, fb_init returns INIT_OK and fb_get_info reports correct dimensions. If VBE unavailable, fb_init returns INIT_FAIL gracefully and fb_get_info returns false.
4. **Keyboard:** all printable keys produce correct ASCII. Shift modifies correctly. E0 keys (arrows, home, end, delete, insert, pgup, pgdn, right ctrl, right alt) all register in key_state[]. KEY_UP events fire on release.
5. **Mouse:** movement updates position. Buttons register. Raw mode accumulates deltas. Sync recovery works (send garbage byte, verify it re-syncs on next valid packet).
6. **Input queue:** push 256 events, verify all 256 are polled back in order. Push 257 events, verify overflow flag is set. Verify no crash.
7. **ATA:** drive detected at init. Read/write round-trip test passes. Retry logic handles a soft reset gracefully. Non-existent drive returns INIT_FAIL.
8. **Serial:** output appears on serial console (QEMU `-serial stdio` or physical serial connection).
9. **Boot display:** trigger each status code (OK, WARN, FAIL). Verify colors. Trigger FATAL, verify system halts.
10. **Integration:** all drivers initialized, boot log shows real status for each. System is stable and responsive to keyboard/mouse input.

---

## Kernel Main Boot Sequence

```c
/* kernel/main.c — AIOS v2 kernel entry point */

void kernel_main(struct boot_info* info) {
    /* ── Early init (no dependencies) ──────────────── */
    serial_init();
    serial_print("\n[AIOS v2] Kernel starting\n");
    vga_init();
    boot_display_banner();

    /* ── Phase 0 validation ────────────────────────── */
    if (info->magic != BOOT_MAGIC) kernel_panic("boot_info magic mismatch!");

    /* ── Phase 1: Memory ──────────────────────────── */
    boot_log("Physical memory manager",      pmm_init(info));
    boot_log("Virtual memory manager",       vmm_init(info));
    boot_log("Kernel heap (slab + buddy)",   heap_init(info));

    /* ── Phase 2: Multitasking ────────────────────── */
    boot_log("GDT with TSS",                gdt_init());
    boot_log("Interrupt descriptor table",   idt_init());
    isr_init();
    boot_log("Exception handlers (ISR 0-31)", INIT_OK);
    irq_init();
    boot_log("Hardware IRQ handlers (PIC)",  INIT_OK);
    fpu_init();
    boot_log("FPU/SSE",                     INIT_OK);
    boot_log("PIT timer (250 Hz)",           timer_init());
    boot_log("Preemptive scheduler",         scheduler_init());

    /* Enable interrupts — timer starts, preemption begins */
    asm volatile("sti");

    /* RDTSC calibration (needs timer running) */
    boot_log("RDTSC calibration",            rdtsc_calibrate());

    /* ── Phase 3: Drivers ─────────────────────────── */
    boot_log("PS/2 keyboard",                keyboard_init());
    boot_log("PS/2 mouse",                   mouse_init());

    /* Framebuffer HAL (optional — ChaosGL needs this) */
    init_result_t fb_result = fb_init(info);
    boot_log("Framebuffer HAL", fb_result);

    boot_log("ATA disk",                     ata_init());

    /* ── Phase 4: ChaosFS ─────────────────────────── */
    boot_log("ChaosFS",                      chaos_mount(CHAOS_LBA_START));

    /* ── Phase 5: ChaosGL ─────────────────────────── */
    if (fb_result == INIT_OK) {
        boot_log("ChaosGL graphics",         chaos_gl_init());
    }

    /* ── Phase 6: KAOS Modules ────────────────────── */
    boot_log("KAOS module manager",          kaos_init());
    kaos_load_all("/modules/");  /* auto-load boot modules */

    /* ── Foundation complete ───────────────────────── */
    boot_print("\nAIOS v2 foundation ready.\n");

    /* ── Continue to shell / desktop ──────────────── */
    /* ... */
}
```

---

## Project Structure

```
AIOS-v2/
├── boot/
│   ├── stage1.asm                 # MBR bootloader (512 bytes)
│   └── stage2.asm                 # Real-mode setup, ELF loader, VBE
├── kernel/
│   ├── main.c                     # Kernel entry point + boot sequence
│   ├── boot_display.c / .h        # Boot logging with real status codes
│   ├── panic.c / .h               # Kernel panic handler
│   ├── gdt.c / gdt.h              # GDT with TSS
│   ├── idt.c / idt.h              # 256-entry IDT
│   ├── isr.c / isr.h              # CPU exception handlers (C)
│   ├── isr_stubs.asm              # Exception entry stubs (NASM)
│   ├── irq.c / irq.h              # Hardware IRQ handlers + PIC (C)
│   ├── irq_stubs.asm              # IRQ entry stubs (NASM)
│   ├── pmm.c / pmm.h              # Physical memory manager (bitmap)
│   ├── vmm.c / vmm.h              # Virtual memory manager (4KB pages)
│   ├── heap.c / heap.h            # Dual allocator: slab + buddy
│   ├── slab.c / slab.h            # Slab allocator internals
│   ├── buddy.c / buddy.h          # Buddy allocator internals
│   ├── scheduler.c / scheduler.h  # Preemptive priority scheduler
│   ├── scheduler_asm.asm          # Context switch (task_switch)
│   ├── rdtsc.c / rdtsc.h          # High-resolution timestamp counter
│   ├── fpu.c / fpu.h              # FPU/SSE initialization
│   └── chaos/                     # ChaosFS (Phase 4)
│       ├── chaos.h / chaos.c      # Public API, lifecycle, fd table
│       ├── chaos_types.h          # All on-disk structs
│       ├── chaos_alloc.c          # Block/inode allocator
│       ├── chaos_dir.c            # Directory operations
│       ├── chaos_inode.c          # Inode cache
│       ├── chaos_block.c          # Block I/O layer
│       ├── chaos_format.c         # Format tool
│       └── chaos_fsck.c           # Consistency checker
├── renderer/                      # ChaosGL (Phase 5) — compiled with RENDERER_CFLAGS (SSE2)
│   ├── chaos_gl.c / chaos_gl.h   # Public C API — single include
│   ├── math.c / math.h           # vec2/3/4, mat4, rect_t
│   ├── backbuffer.c / .h         # Back buffer + z-buffer, begin/end_frame
│   ├── 2d.c / 2d.h               # 2D primitives, clip stack, text, blit
│   ├── font.c / font.h           # Claude Mono 8×16 bitmap font
│   ├── pipeline.c / .h           # Vertex transform, clip, perspective divide
│   ├── rasterizer.c / .h         # Triangle rasterization, z-test
│   ├── shaders.c / .h            # Built-in shaders + registry
│   ├── texture.c / .h            # Texture table, load (.raw), sample
│   ├── model.c / .h              # .cobj load/free
│   └── lua_bindings.c            # Lua API registration
├── drivers/
│   ├── timer.c / timer.h          # PIT timer (250 Hz)
│   ├── vga.c / vga.h              # VGA text mode
│   ├── framebuffer.c / .h         # VBE framebuffer HAL (fb_get_info only)
│   ├── keyboard.c / keyboard.h    # PS/2 keyboard with E0 support
│   ├── mouse.c / mouse.h          # PS/2 mouse with raw mode
│   ├── input.c / input.h          # Shared input event queue
│   ├── ata.c / ata.h              # ATA/IDE PIO disk
│   └── serial.c / serial.h        # Serial debug output
├── include/
│   ├── boot_info.h                # boot_info struct (shared boot/kernel)
│   ├── types.h                    # stdint, bool, size_t, etc.
│   ├── string.h / string.c        # memcpy, memset, strlen, etc.
│   └── io.h                       # inb, outb, io_wait, etc.
├── tools/                         # Host-side build tools
│   ├── obj2cobj.c                 # .obj → .cobj model converter
│   └── gen_texture.c              # Test texture generator
├── documents/
│   ├── AIOS-v2-foundation-spec-revised.md   # This document
│   ├── ChaosFS-v2-spec-revised.md           # Filesystem spec
│   ├── ChaosGL-spec.md                      # Graphics subsystem spec
│   └── KAOS-module-spec.md                  # Module system spec
├── linker.ld                      # Kernel linker script (linked at 0x100000)
└── Makefile                       # Build system
```

---

## Build System Requirements

```makefile
# Cross-compiler
CC = i686-elf-gcc
AS = nasm
LD = i686-elf-ld

# Kernel/driver code — compiler must NOT emit SSE/MMX/SSE2.
# Reason: IRQ handlers run without fxsave/fxrstor protection. Compiler-emitted
# SSE in kernel code would silently clobber task XMM registers mid-interrupt.
CFLAGS = -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror \
         -O2 -g -std=c11 -march=core2 -mno-sse -mno-mmx -mno-sse2 -I.

# Renderer module (ChaosGL) — SSE2 allowed for SIMD inner loops.
# ONLY for files under renderer/. Never kernel/, drivers/, or boot/.
RENDERER_CFLAGS = -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror \
                  -O2 -g -std=c11 -march=core2 -msse2 -mfpmath=sse -I.

# Linker uses $(CC) not $(LD) so it auto-finds libgcc.a for 64-bit helpers
LDFLAGS = -T linker.ld -nostdlib

# Stage 2 size check (MUST be <= 8192 bytes)
stage2_check:
	@SIZE=$$(wc -c < build/stage2.bin | tr -d ' '); \
	if [ $$SIZE -gt 8192 ]; then \
		echo "ERROR: Stage 2 is $$SIZE bytes (max 8192)"; exit 1; \
	fi
```

**Two CFLAGS sets are mandatory.** Kernel code uses `-mno-sse` to prevent the compiler from generating SSE instructions that would corrupt FPU state during interrupts. Renderer code uses `-msse2 -mfpmath=sse` for fast float math. The scheduler's fxsave/fxrstor protects renderer tasks' XMM registers across context switches.

---

## Phase Roadmap

| Phase | Subsystem | Spec Document | Dependencies | Status |
|-------|-----------|---------------|-------------|--------|
| 0 | Bootloader | This document | — | PASS (13/13) |
| 1 | Memory (PMM, VMM, Heap) | This document | Phase 0 | PASS (15/15) |
| 2 | Multitasking (GDT, IDT, ISR, IRQ, Timer, FPU, Scheduler) | This document | Phase 1 | PASS (12/12) |
| 3 | Drivers (Keyboard, Mouse, Input, ATA, Framebuffer HAL) | This document | Phase 2 | — |
| 4 | ChaosFS (filesystem) | `ChaosFS-v2-spec-revised.md` | Phase 3 (ATA) | — |
| 5 | ChaosGL (universal graphics) | `ChaosGL-spec.md` | Phase 3 (fb HAL), Phase 4 (texture/model loading) | — |
| 6 | KAOS Modules (kernel add-on system) | `KAOS-module-spec.md` | Phase 4 (ChaosFS for .kaos files) | — |
| 7+ | Shell, Lua, Desktop, Applications | TBD | Phase 4-6 | — |

**Dependency notes:**
- ChaosGL core (back buffer, 2D primitives, 3D pipeline) only needs Phase 3 (framebuffer HAL).
- ChaosGL texture/model loading additionally needs Phase 4 (ChaosFS) for reading .raw and .cobj files from disk.
- KAOS modules live on ChaosFS as `.kaos` files, so Phase 4 must be complete before Phase 6.
- The framebuffer driver is HAL-only (Section 3.3). All drawing goes through ChaosGL. See `ChaosGL-spec.md`.

---

## Summary

This spec covers the foundational phases (0-3) of AIOS v2.

| Phase | Components | Key Improvements Over v1 |
|-------|-----------|--------------------------|
| 0 | Bootloader | ELF streaming load, boot_info struct, VBE in real mode, disk retry, explicit entry ABI |
| 1 | Memory | Dynamic PMM, selective VMM mapping, slab+buddy heap, page ownership, aligned alloc |
| 2 | Multitasking | GDT/TSS, IDT, ISR/IRQ stubs, PIC remap, FPU save/restore, priority scheduler, starvation prevention, entry trampoline, re-entrancy protection |
| 3 | Drivers | Framebuffer HAL (no drawing — ChaosGL owns rendering), E0 scancodes, mouse raw mode, input queue, ATA retry |

**v1 issues addressed:**

| Issue | Fix |
|-------|-----|
| boot_info too large for 0x0500 | Moved to 0x10000 (4KB page) with static assert |
| Stage 2 size/address inconsistency | Memory map and disk layout both agree: 8KB (16 sectors), 0x07E00-0x09DFF |
| ELF p_vaddr ambiguity | Explicit contract: kernel linked at phys 0x100000, loader uses p_paddr |
| PMM assumes contiguous kernel | Per-segment reservation from boot_info.kernel_segments[] |
| Identity map everything wasteful | Selective mapping of only needed regions |
| SLAB_MAGIC for allocator routing | Page ownership table — O(1) lookup, no magic sniffing |
| kmalloc_aligned unspecified | Full contract: over-allocate + store original pointer, restrictions documented |
| Scheduler cleanup lifecycle | Explicit 7-step lifecycle contract, deferred cleanup |
| CPU usage measurement wrong | Idle task ticks vs total ticks in completed windows |
| 32-bit tick wrap | 64-bit counter (wraps in 2.3 billion years) |
| FPU not saved on context switch | fxsave/fxrstor on every switch, separate aligned buffer per task |
| New tasks start with IF=0 | Entry trampoline does `sti` before jumping to task entry function |
| schedule() re-entrancy | pushf/cli guard prevents timer IRQ during yield-triggered schedule |
| Tail-call corrupts task_switch | Compiler barrier + noinline on schedule() |
| Framebuffer had drawing primitives | Demoted to HAL-only (fb_get_info). ChaosGL is the universal graphics API |
| Keyboard missing E0 scancodes | E0 prefix state machine, remapped to 128+ range |
| Input queue concurrency | Explicit contract: IRQ serialization, cli/sti on poll, overflow policy |

**Do not proceed to Phase 4 until all Phase 0-3 acceptance tests pass.**
