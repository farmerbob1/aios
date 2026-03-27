/* AIOS UEFI Bootloader
 *
 * Replaces the BIOS stage1/stage2 bootloader. Runs as x86_64 PE/COFF EFI
 * application under EDK2 firmware. After loading the kernel and populating
 * boot_info, transitions from 64-bit long mode to 32-bit protected mode
 * and jumps to kernel_main — leaving the machine in exactly the same state
 * as the old BIOS bootloader.
 *
 * Compiled with: x86_64-w64-mingw32-gcc (MinGW, MS ABI default)
 */

#include "uefi.h"

/* ================================================================
 * Constants
 * ================================================================ */

#define BOOT_INFO_PHYS      0x10000
#define KERNEL_LOAD_BASE    0x100000
#define SERIAL_PORT         0x3F8

#define BOOT_MAGIC          0x434C4F53  /* 'CLOS' */
#define MAX_E820            32
#define MAX_VBE_MODES       32
#define MAX_KERNEL_SEGMENTS 8

/* ELF32 constants */
#define ELF_MAGIC       0x464C457F  /* "\x7FELF" */
#define ET_EXEC         2
#define EM_386          3
#define PT_LOAD         1

/* ================================================================
 * boot_info struct — MUST match include/boot_info.h exactly
 * ================================================================ */

#pragma pack(push, 1)

typedef struct {
    UINT64 base;
    UINT64 length;
    UINT32 type;
    UINT32 acpi;
} E820Entry;

typedef struct {
    UINT16 width;
    UINT16 height;
    UINT16 pitch;
    UINT16 mode_number;
    UINT32 fb_addr;
    UINT8  bpp;
    UINT8  reserved[3];
} VBEModeEntry;

typedef struct {
    UINT32 phys_start;
    UINT32 phys_end;
} KernelSegment;

typedef struct {
    UINT32       magic;
    UINT32       version;
    UINT32       e820_count;
    E820Entry    e820_entries[MAX_E820];
    UINT32       max_phys_addr;
    UINT32       kernel_entry;
    UINT32       kernel_phys_start;
    UINT32       kernel_phys_end;
    UINT32       kernel_loaded_bytes;
    UINT32       kernel_segment_count;
    KernelSegment kernel_segments[MAX_KERNEL_SEGMENTS];
    UINT32       fb_addr;
    UINT32       fb_width;
    UINT32       fb_height;
    UINT32       fb_pitch;
    UINT8        fb_bpp;
    UINT8        fb_pad[3];
    UINT16       vbe_mode_count;
    UINT16       vbe_current_mode;
    VBEModeEntry vbe_modes[MAX_VBE_MODES];
    UINT32       boot_flags;
    UINT32       acpi_rsdp;
} BootInfo;

/* ELF32 headers */
typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT32 e_entry;
    UINT32 e_phoff;
    UINT32 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_offset;
    UINT32 p_vaddr;
    UINT32 p_paddr;
    UINT32 p_filesz;
    UINT32 p_memsz;
    UINT32 p_flags;
    UINT32 p_align;
} Elf32_Phdr;

#pragma pack(pop)

/* ================================================================
 * Protocol GUIDs
 * ================================================================ */

EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_GUID;

/* ================================================================
 * Global State
 * ================================================================ */

static EFI_SYSTEM_TABLE  *gST;
static EFI_BOOT_SERVICES *gBS;
static EFI_HANDLE         gImageHandle;

/* ================================================================
 * Utility Functions
 * ================================================================ */

static void *memset_boot(void *s, int c, UINTN n) {
    UINT8 *p = (UINT8*)s;
    while (n--) *p++ = (UINT8)c;
    return s;
}


/* ================================================================
 * Serial I/O (COM1 0x3F8, direct port access)
 * ================================================================ */

static inline void outb(UINT16 port, UINT8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline UINT8 inb(UINT16 port) {
    UINT8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(SERIAL_PORT + 1, 0x00);  /* Disable interrupts */
    outb(SERIAL_PORT + 3, 0x80);  /* DLAB on */
    outb(SERIAL_PORT + 0, 0x01);  /* 115200 baud (divisor low) */
    outb(SERIAL_PORT + 1, 0x00);  /* divisor high */
    outb(SERIAL_PORT + 3, 0x03);  /* 8N1 */
    outb(SERIAL_PORT + 2, 0xC7);  /* FIFO */
    outb(SERIAL_PORT + 4, 0x0B);  /* DTR + RTS + OUT2 */
}

static void serial_putchar(char c) {
    while (!(inb(SERIAL_PORT + 5) & 0x20))
        ;
    outb(SERIAL_PORT, (UINT8)c);
}

static void serial_print(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putchar('\r');
        serial_putchar(*s++);
    }
}

static void serial_print_hex(UINT64 val) {
    const char hex[] = "0123456789ABCDEF";
    char buf[17];
    buf[16] = 0;
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    /* Skip leading zeros but always print at least one digit */
    const char *p = buf;
    while (*p == '0' && p[1]) p++;
    serial_print("0x");
    serial_print(p);
}

static void serial_print_dec(UINT32 val) {
    char buf[11];
    int i = 10;
    buf[i] = 0;
    if (val == 0) { serial_putchar('0'); return; }
    while (val > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    serial_print(&buf[i]);
}

/* ================================================================
 * Console Output (UEFI text mode, for visual feedback)
 * ================================================================ */

static void print(const CHAR16 *s) {
    gST->ConOut->OutputString(gST->ConOut, (CHAR16*)s);
}

/* ================================================================
 * Memory Map Acquisition
 *
 * Gets UEFI memory map and converts to E820 format.
 * Returns the map key needed for ExitBootServices().
 * ================================================================ */

static UINT32 efi_to_e820_type(UINT32 efi_type) {
    switch (efi_type) {
    case EfiConventionalMemory:
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiPersistentMemory:
        return 1;  /* Usable */
    case EfiACPIReclaimMemory:
        return 3;  /* ACPI Reclaimable */
    case EfiACPIMemoryNVS:
        return 4;  /* ACPI NVS */
    case EfiUnusableMemory:
        return 5;  /* Bad memory */
    default:
        return 2;  /* Reserved */
    }
}

static UINTN get_memory_map(BootInfo *bi, UINTN *map_key_out) {
    EFI_STATUS status;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    UINT32 desc_ver = 0;
    EFI_MEMORY_DESCRIPTOR *map = (void*)0;

    /* First call to get required size */
    status = gBS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
    /* Expected: EFI_BUFFER_TOO_SMALL */
    map_size += 4096;  /* Extra space for the allocation itself */

    status = gBS->AllocatePool(EfiLoaderData, map_size, (VOID**)&map);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: AllocatePool for memory map failed\n");
        return 0;
    }

    status = gBS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: GetMemoryMap failed\n");
        gBS->FreePool(map);
        return 0;
    }

    /* Convert to E820 entries */
    UINTN num_entries = map_size / desc_size;
    UINT32 e820_count = 0;
    UINT64 max_phys = 0;

    for (UINTN i = 0; i < num_entries && e820_count < MAX_E820; i++) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR*)((UINT8*)map + i * desc_size);

        UINT64 base = desc->PhysicalStart;
        UINT64 length = desc->NumberOfPages * 4096ULL;
        UINT32 type = efi_to_e820_type(desc->Type);

        /* Try to merge with previous entry if same type and contiguous */
        if (e820_count > 0) {
            E820Entry *prev = &bi->e820_entries[e820_count - 1];
            if (prev->type == type && prev->base + prev->length == base) {
                prev->length += length;
                if (base + length > max_phys) max_phys = base + length;
                continue;
            }
        }

        bi->e820_entries[e820_count].base = base;
        bi->e820_entries[e820_count].length = length;
        bi->e820_entries[e820_count].type = type;
        bi->e820_entries[e820_count].acpi = 0;
        e820_count++;

        if (base + length > max_phys)
            max_phys = base + length;
    }

    bi->e820_count = e820_count;
    bi->max_phys_addr = (UINT32)(max_phys > 0xFFFFFFFF ? 0xFFFFFFFF : max_phys);

    serial_print("[UEFI] Memory map: ");
    serial_print_dec(e820_count);
    serial_print(" E820 entries, max phys=");
    serial_print_hex(max_phys);
    serial_print("\n");

    *map_key_out = map_key;

    /* Don't free the map — we need map_key to remain valid for ExitBootServices.
     * GetMemoryMap must be the LAST boot services call before ExitBootServices. */

    return e820_count;
}

/* ================================================================
 * GOP Framebuffer Setup
 * ================================================================ */

static EFI_STATUS setup_gop(BootInfo *bi) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;

    status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, (VOID*)0, (VOID**)&gop);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] GOP not found, no framebuffer\n");
        bi->fb_addr = 0;
        return status;
    }

    /* Score modes: prefer 1024x768@32bpp */
    UINT32 best_mode = gop->Mode->Mode;  /* current mode as fallback */
    INT32 best_score = -1;
    UINT32 mode_count = 0;

    for (UINT32 i = 0; i < gop->Mode->MaxMode && mode_count < MAX_VBE_MODES; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN info_size;

        status = gop->QueryMode(gop, i, &info_size, &info);
        if (EFI_ERROR(status)) continue;

        /* Only accept 32-bit pixel formats */
        BOOLEAN is_32bpp = (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
                            info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor);
        if (!is_32bpp) continue;

        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;
        UINT32 pitch = info->PixelsPerScanLine * 4;

        /* Score the mode */
        INT32 score = 1;
        if (w == 1024 && h == 768) score = 4;
        else if (w == 800 && h == 600) score = 3;
        else if (w == 640 && h == 480) score = 2;

        if (score > best_score) {
            best_score = score;
            best_mode = i;
        }

        /* Record in VBE modes list */
        bi->vbe_modes[mode_count].width = (UINT16)w;
        bi->vbe_modes[mode_count].height = (UINT16)h;
        bi->vbe_modes[mode_count].pitch = (UINT16)pitch;
        bi->vbe_modes[mode_count].mode_number = (UINT16)i;
        bi->vbe_modes[mode_count].fb_addr = (UINT32)gop->Mode->FrameBufferBase;
        bi->vbe_modes[mode_count].bpp = 32;
        memset_boot(bi->vbe_modes[mode_count].reserved, 0, 3);
        mode_count++;
    }

    bi->vbe_mode_count = (UINT16)mode_count;

    /* Set the best mode */
    status = gop->SetMode(gop, best_mode);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] SetMode failed, using current mode\n");
    }

    bi->vbe_current_mode = (UINT16)best_mode;

    /* Read framebuffer info from the (now active) mode */
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *active = gop->Mode->Info;
    bi->fb_addr   = (UINT32)gop->Mode->FrameBufferBase;
    bi->fb_width  = active->HorizontalResolution;
    bi->fb_height = active->VerticalResolution;
    bi->fb_pitch  = active->PixelsPerScanLine * 4;
    bi->fb_bpp    = 32;
    memset_boot(bi->fb_pad, 0, 3);

    serial_print("[UEFI] GOP: ");
    serial_print_dec(bi->fb_width);
    serial_print("x");
    serial_print_dec(bi->fb_height);
    serial_print(" @ ");
    serial_print_hex(bi->fb_addr);
    serial_print("\n");

    return EFI_SUCCESS;
}

/* ================================================================
 * Kernel ELF Loader
 *
 * Loads kernel.elf from the ESP filesystem root, parses ELF32
 * headers, and copies PT_LOAD segments to physical memory.
 * ================================================================ */

static EFI_STATUS load_kernel(BootInfo *bi) {
    EFI_STATUS status;

    /* Get our loaded image to find the device we booted from */
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    status = gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid,
                                 (VOID**)&loaded_image);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: Cannot get LoadedImage protocol\n");
        return status;
    }

    /* Get the filesystem from the boot device */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    status = gBS->HandleProtocol(loaded_image->DeviceHandle,
                                 &gEfiSimpleFileSystemProtocolGuid, (VOID**)&fs);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: Cannot get SimpleFileSystem protocol\n");
        return status;
    }

    /* Open root directory */
    EFI_FILE_PROTOCOL *root;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: Cannot open volume root\n");
        return status;
    }

    /* Open kernel.elf */
    EFI_FILE_PROTOCOL *kernel_file;
    status = root->Open(root, &kernel_file, u"kernel.elf",
                        EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: Cannot open \\kernel.elf\n");
        root->Close(root);
        return status;
    }

    serial_print("[UEFI] Loading kernel.elf...\n");

    /* Read ELF header */
    Elf32_Ehdr ehdr;
    UINTN read_size = sizeof(ehdr);
    status = kernel_file->Read(kernel_file, &read_size, &ehdr);
    if (EFI_ERROR(status) || read_size != sizeof(ehdr)) {
        serial_print("[UEFI] FATAL: Cannot read ELF header\n");
        goto fail;
    }

    /* Validate ELF header */
    UINT32 elf_magic = *(UINT32*)ehdr.e_ident;
    if (elf_magic != ELF_MAGIC) {
        serial_print("[UEFI] FATAL: Bad ELF magic\n");
        goto fail;
    }
    if (ehdr.e_ident[4] != 1 || ehdr.e_type != ET_EXEC || ehdr.e_machine != EM_386) {
        serial_print("[UEFI] FATAL: Not a 32-bit x86 executable\n");
        goto fail;
    }

    serial_print("[UEFI] ELF entry=");
    serial_print_hex(ehdr.e_entry);
    serial_print(" phnum=");
    serial_print_dec(ehdr.e_phnum);
    serial_print("\n");

    /* Read program headers */
    UINTN ph_size = (UINTN)ehdr.e_phnum * ehdr.e_phentsize;
    Elf32_Phdr *phdrs;
    status = gBS->AllocatePool(EfiLoaderData, ph_size, (VOID**)&phdrs);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: Cannot allocate for program headers\n");
        goto fail;
    }

    /* Seek to program header offset */
    status = kernel_file->SetPosition(kernel_file, ehdr.e_phoff);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: Cannot seek to program headers\n");
        gBS->FreePool(phdrs);
        goto fail;
    }

    read_size = ph_size;
    status = kernel_file->Read(kernel_file, &read_size, phdrs);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: Cannot read program headers\n");
        gBS->FreePool(phdrs);
        goto fail;
    }

    /* Load each PT_LOAD segment */
    bi->kernel_entry = ehdr.e_entry;
    bi->kernel_phys_start = 0xFFFFFFFF;
    bi->kernel_phys_end = 0;
    bi->kernel_loaded_bytes = 0;
    bi->kernel_segment_count = 0;

    for (UINT16 i = 0; i < ehdr.e_phnum && bi->kernel_segment_count < MAX_KERNEL_SEGMENTS; i++) {
        Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        UINT32 seg_start = ph->p_paddr & ~0xFFF;  /* page-align down */
        UINT32 seg_end = (ph->p_paddr + ph->p_memsz + 0xFFF) & ~0xFFF;  /* page-align up */
        UINTN num_pages = (seg_end - seg_start) / 4096;

        serial_print("[UEFI]   PT_LOAD: paddr=");
        serial_print_hex(ph->p_paddr);
        serial_print(" filesz=");
        serial_print_dec(ph->p_filesz);
        serial_print(" memsz=");
        serial_print_dec(ph->p_memsz);
        serial_print("\n");

        /* Allocate physical pages at the exact target address */
        EFI_PHYSICAL_ADDRESS target = seg_start;
        status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, num_pages, &target);
        if (EFI_ERROR(status)) {
            serial_print("[UEFI] FATAL: Cannot allocate pages at ");
            serial_print_hex(seg_start);
            serial_print("\n");
            gBS->FreePool(phdrs);
            goto fail;
        }

        /* Zero the entire region first (handles BSS) */
        memset_boot((void*)(UINTN)seg_start, 0, num_pages * 4096);

        /* Read file data into the target physical address */
        if (ph->p_filesz > 0) {
            status = kernel_file->SetPosition(kernel_file, ph->p_offset);
            if (EFI_ERROR(status)) {
                serial_print("[UEFI] FATAL: Cannot seek to segment data\n");
                gBS->FreePool(phdrs);
                goto fail;
            }

            read_size = ph->p_filesz;
            status = kernel_file->Read(kernel_file, &read_size,
                                       (void*)(UINTN)ph->p_paddr);
            if (EFI_ERROR(status)) {
                serial_print("[UEFI] FATAL: Cannot read segment data\n");
                gBS->FreePool(phdrs);
                goto fail;
            }
        }

        /* Track kernel extents */
        if (seg_start < bi->kernel_phys_start) bi->kernel_phys_start = seg_start;
        if (seg_end > bi->kernel_phys_end)     bi->kernel_phys_end = seg_end;
        bi->kernel_loaded_bytes += ph->p_filesz;

        bi->kernel_segments[bi->kernel_segment_count].phys_start = seg_start;
        bi->kernel_segments[bi->kernel_segment_count].phys_end = seg_end;
        bi->kernel_segment_count++;
    }

    gBS->FreePool(phdrs);

    serial_print("[UEFI] Kernel loaded: ");
    serial_print_hex(bi->kernel_phys_start);
    serial_print(" - ");
    serial_print_hex(bi->kernel_phys_end);
    serial_print(" (");
    serial_print_dec(bi->kernel_loaded_bytes);
    serial_print(" bytes)\n");

    kernel_file->Close(kernel_file);
    root->Close(root);
    return EFI_SUCCESS;

fail:
    kernel_file->Close(kernel_file);
    root->Close(root);
    return EFI_LOAD_ERROR;
}

/* ================================================================
 * Mode Transition (defined in transition.asm)
 *
 * Transitions from x86_64 long mode to i686 32-bit protected mode,
 * then jumps to kernel_main(boot_info*). Never returns.
 * ================================================================ */

extern void enter_32bit_and_jump(UINT64 kernel_entry, UINT64 boot_info_ptr)
    __attribute__((noreturn));

/* ================================================================
 * EFI Entry Point
 * ================================================================ */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *systab) {
    EFI_STATUS status;

    gST = systab;
    gBS = systab->BootServices;
    gImageHandle = image_handle;

    /* Disable watchdog timer (default 5-minute reset) */
    gBS->SetWatchdogTimer(0, 0, 0, (CHAR16*)0);

    /* Initialize serial for debug output */
    serial_init();
    serial_print("\n[UEFI] AIOS UEFI Bootloader starting\n");

    /* Console feedback */
    gST->ConOut->ClearScreen(gST->ConOut);
    print(u"AIOS UEFI Bootloader\r\n");

    /* Allocate boot_info page at fixed physical address */
    EFI_PHYSICAL_ADDRESS bi_addr = BOOT_INFO_PHYS;
    status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, 1, &bi_addr);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: Cannot allocate boot_info page at 0x10000\n");
        print(u"FATAL: Cannot allocate boot_info page\r\n");
        return status;
    }

    BootInfo *bi = (BootInfo*)(UINTN)BOOT_INFO_PHYS;
    memset_boot(bi, 0, 4096);

    /* ── Step 1: GOP framebuffer ─────────────────────── */
    setup_gop(bi);

    /* ── Step 2: Load kernel ELF ─────────────────────── */
    status = load_kernel(bi);
    if (EFI_ERROR(status)) {
        serial_print("[UEFI] FATAL: Kernel load failed\n");
        print(u"FATAL: Kernel load failed\r\n");
        return status;
    }

    /* ── Step 3: Retrieve ACPI RSDP from System Table ── */
    {
        /* ACPI 2.0+ RSDP GUID: {8868e871-e4f1-11d3-bc22-0080c73c8881} */
        EFI_GUID acpi20_guid = { 0x8868e871, 0xe4f1, 0x11d3,
            { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
        /* ACPI 1.0 RSDP GUID: {eb9d2d30-2d88-11d3-9a16-0090273fc14d} */
        EFI_GUID acpi10_guid = { 0xeb9d2d30, 0x2d88, 0x11d3,
            { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };

        bi->acpi_rsdp = 0;
        /* Prefer ACPI 2.0+ XSDP, fall back to ACPI 1.0 RSDP */
        for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
            EFI_CONFIGURATION_TABLE *ct = &gST->ConfigurationTable[i];
            if (guid_eq(&ct->VendorGuid, &acpi20_guid)) {
                bi->acpi_rsdp = (UINT32)(UINTN)ct->VendorTable;
                serial_print("[UEFI] Found ACPI 2.0+ RSDP\n");
                break;
            }
            if (guid_eq(&ct->VendorGuid, &acpi10_guid) && bi->acpi_rsdp == 0) {
                bi->acpi_rsdp = (UINT32)(UINTN)ct->VendorTable;
                serial_print("[UEFI] Found ACPI 1.0 RSDP\n");
            }
        }
        if (bi->acpi_rsdp == 0) {
            serial_print("[UEFI] WARNING: No ACPI RSDP found in configuration table\n");
        }
    }

    /* ── Step 4: Finalize boot_info ──────────────────── */
    bi->magic = BOOT_MAGIC;
    bi->version = 1;
    bi->boot_flags = 1;  /* Flag 1 = UEFI boot (informational) */

    serial_print("[UEFI] boot_info populated at 0x10000\n");
    print(u"Kernel loaded. Entering kernel...\r\n");

    /* ── Step 5: Get memory map (MUST be last BS call) ─ */
    UINTN map_key = 0;
    UINTN e820_count = get_memory_map(bi, &map_key);
    if (e820_count == 0) {
        serial_print("[UEFI] FATAL: Empty memory map\n");
        return EFI_LOAD_ERROR;
    }

    /* ── Step 6: Exit Boot Services ──────────────────── */
    status = gBS->ExitBootServices(gImageHandle, map_key);
    if (EFI_ERROR(status)) {
        /* Map key may have changed, retry once */
        serial_print("[UEFI] ExitBootServices retry...\n");

        UINTN map_size = 0, desc_size = 0;
        UINT32 desc_ver = 0;
        EFI_MEMORY_DESCRIPTOR *map2 = (void*)0;

        gBS->GetMemoryMap(&map_size, map2, &map_key, &desc_size, &desc_ver);
        map_size += 4096;
        gBS->AllocatePool(EfiLoaderData, map_size, (VOID**)&map2);
        gBS->GetMemoryMap(&map_size, map2, &map_key, &desc_size, &desc_ver);
        status = gBS->ExitBootServices(gImageHandle, map_key);

        if (EFI_ERROR(status)) {
            serial_print("[UEFI] FATAL: ExitBootServices failed\n");
            return status;
        }
    }

    /* ════════════════════════════════════════════════════
     * NO MORE UEFI CALLS AFTER THIS POINT.
     * We own the machine. Interrupts are disabled.
     * Memory is identity-mapped (OVMF default).
     * ════════════════════════════════════════════════════ */

    serial_print("[UEFI] Boot services exited. Entering 32-bit mode...\n");

    /* ── Step 7: Transition to 32-bit and jump to kernel ─ */
    enter_32bit_and_jump(bi->kernel_entry, BOOT_INFO_PHYS);

    /* Never reached */
    __builtin_unreachable();
}
