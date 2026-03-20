; =============================================================================
; AIOS v2 — Stage 2 Bootloader (max 8KB, real mode)
;
; Runs in real mode. Does everything that needs BIOS services:
;   1. Save boot drive
;   2. E820 memory map
;   3. VBE mode setup
;   4. Enter unreal mode (for accessing memory above 1MB)
;   5. Load kernel ELF from disk
;   6. Finalize boot_info
;   7. Enter protected mode and jump to kernel
;
; DESIGN CONTRACT: boot_info is at physical 0x10000 (4KB page).
; DESIGN CONTRACT: Kernel ELF starts at disk sector 17.
; DESIGN CONTRACT: Kernel loads at physical 0x100000 (1MB mark).
;
; All addresses > 64KB are accessed via segment:offset pairs.
; =============================================================================

[BITS 16]
[ORG 0x7E00]

; =============================================================================
; boot_info field offsets (must match include/boot_info.h exactly)
; Accessed via ES=0x1000, offset=BI_*
; =============================================================================
BI_MAGIC                    equ 0
BI_VERSION                  equ 4
BI_E820_COUNT               equ 8
BI_E820_ENTRIES             equ 12
BI_MAX_PHYS_ADDR            equ 780
BI_KERNEL_ENTRY             equ 784
BI_KERNEL_PHYS_START        equ 788
BI_KERNEL_PHYS_END          equ 792
BI_KERNEL_LOADED_BYTES      equ 796
BI_KERNEL_SEGMENT_COUNT     equ 800
BI_KERNEL_SEGMENTS          equ 804
BI_FB_ADDR                  equ 868
BI_FB_WIDTH                 equ 872
BI_FB_HEIGHT                equ 876
BI_FB_PITCH                 equ 880
BI_FB_BPP                   equ 884
BI_VBE_MODE_COUNT           equ 888
BI_VBE_CURRENT_MODE         equ 890
BI_VBE_MODES                equ 892
BI_BOOT_FLAGS               equ 1404

; Segment constants for accessing memory above 64KB
BOOTINFO_SEG                equ 0x1000   ; physical 0x10000
VBE_INFO_SEG                equ 0x1100   ; physical 0x11000
VBE_MODE_SEG                equ 0x1120   ; physical 0x11200
SCRATCH_SEG                 equ 0x2000   ; physical 0x20000

; Constants
BOOT_MAGIC_VAL              equ 0x434C4F53   ; 'CLOS'
KERNEL_SECTOR_START         equ 17
KERNEL_LOAD_ADDR            equ 0x100000
KERNEL_MAX_ADDR             equ 0x1000000     ; 16MB sanity limit

; ELF constants
ELF_MAGIC                   equ 0x464C457F    ; 0x7F 'E' 'L' 'F'
PT_LOAD                     equ 1

; =============================================================================
; Entry point
; =============================================================================
stage2_entry:
    ; Step 1: Save boot drive (DL from Stage 1)
    mov [boot_drive], dl

    ; Print entry message to serial
    call serial_print_msg
    db "Stage 2 entry", 13, 10, 0

    ; Zero out the boot_info region (4KB at physical 0x10000)
    mov ax, BOOTINFO_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 2048            ; 4096 / 2 words
    rep stosw
    ; Restore ES=0
    xor ax, ax
    mov es, ax

    ; -----------------------------------------------------------------
    ; Step 2: E820 Memory Map
    ; -----------------------------------------------------------------
    call serial_print_msg
    db "E820 memory map...", 13, 10, 0

    call do_e820
    jnc .e820_ok

    call serial_print_msg
    db "E820 failed, trying E801...", 13, 10, 0
    call do_e801_fallback
    jnc .e820_ok

    call serial_print_msg
    db "E801 failed, assuming 64MB", 13, 10, 0
    call assume_64mb

.e820_ok:
    call compute_max_phys_addr

    call serial_print_msg
    db "E820 done", 13, 10, 0

    ; -----------------------------------------------------------------
    ; Step 3: VBE Mode Setup
    ; -----------------------------------------------------------------
    call serial_print_msg
    db "VBE mode setup...", 13, 10, 0

    call do_vbe

    call serial_print_msg
    db "VBE done", 13, 10, 0

    ; -----------------------------------------------------------------
    ; Step 4: Enter Unreal Mode
    ; -----------------------------------------------------------------
    call enter_unreal_mode

    call serial_print_msg
    db "Unreal mode active", 13, 10, 0

    ; -----------------------------------------------------------------
    ; Step 5: Load Kernel ELF
    ; -----------------------------------------------------------------
    call serial_print_msg
    db "Loading kernel ELF...", 13, 10, 0

    call do_elf_load

    call serial_print_msg
    db "Kernel loaded", 13, 10, 0

    ; -----------------------------------------------------------------
    ; Step 6: Finalize boot_info
    ; -----------------------------------------------------------------
    mov ax, BOOTINFO_SEG
    mov es, ax
    mov dword [es:BI_MAGIC], BOOT_MAGIC_VAL
    mov dword [es:BI_VERSION], 1
    mov dword [es:BI_BOOT_FLAGS], 0
    xor ax, ax
    mov es, ax

    ; -----------------------------------------------------------------
    ; Step 7: Enter Protected Mode
    ; -----------------------------------------------------------------
    call serial_print_msg
    db "Entering protected mode...", 13, 10, 0

    call enter_protected_mode
    ; Never returns


; =============================================================================
; Serial Output (COM1 = 0x3F8)
; =============================================================================
SERIAL_PORT equ 0x3F8

serial_putchar:
    push dx
    push ax
    mov dx, SERIAL_PORT + 5
.wait:
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, SERIAL_PORT
    out dx, al
    pop dx
    ret

; Print inline null-terminated string after CALL
serial_print_msg:
    pop si
.loop:
    lodsb
    test al, al
    jz .done
    call serial_putchar
    jmp .loop
.done:
    push si
    ret


; =============================================================================
; E820 Memory Map
; CF clear = success, CF set = failure
; =============================================================================
do_e820:
    mov ax, BOOTINFO_SEG
    mov es, ax
    mov di, BI_E820_ENTRIES
    xor ebx, ebx
    mov dword [es:BI_E820_COUNT], 0

.loop:
    mov eax, 0x0000E820
    mov ecx, 24
    mov edx, 0x534D4150
    int 0x15
    jc .fail
    cmp eax, 0x534D4150
    jne .fail

    inc dword [es:BI_E820_COUNT]
    add di, 24

    cmp dword [es:BI_E820_COUNT], 32
    jge .done
    test ebx, ebx
    jnz .loop

.done:
    ; Restore ES
    xor ax, ax
    mov es, ax
    clc
    ret

.fail:
    xor ax, ax
    mov es, ax
    stc
    ret


; =============================================================================
; E801 Fallback
; =============================================================================
do_e801_fallback:
    mov ax, 0xE801
    int 0x15
    jc .fail

    test cx, cx
    jnz .use_cx_dx
    mov cx, ax
    mov dx, bx
.use_cx_dx:

    push es
    mov ax, BOOTINFO_SEG
    mov es, ax

    ; Entry 0: low memory 0x0 - 0x9FFFF (640KB)
    mov di, BI_E820_ENTRIES
    mov dword [es:di + 0], 0
    mov dword [es:di + 4], 0
    mov dword [es:di + 8], 0x000A0000
    mov dword [es:di + 12], 0
    mov dword [es:di + 16], 1
    mov dword [es:di + 20], 0

    ; Entry 1: 1MB to 16MB
    add di, 24
    mov dword [es:di + 0], 0x00100000
    mov dword [es:di + 4], 0
    movzx eax, cx
    shl eax, 10
    mov dword [es:di + 8], eax
    mov dword [es:di + 12], 0
    mov dword [es:di + 16], 1
    mov dword [es:di + 20], 0

    mov dword [es:BI_E820_COUNT], 2

    test dx, dx
    jz .done

    ; Entry 2: above 16MB
    add di, 24
    mov dword [es:di + 0], 0x01000000
    mov dword [es:di + 4], 0
    movzx eax, dx
    shl eax, 16
    mov dword [es:di + 8], eax
    mov dword [es:di + 12], 0
    mov dword [es:di + 16], 1
    mov dword [es:di + 20], 0

    mov dword [es:BI_E820_COUNT], 3

.done:
    pop es
    clc
    ret

.fail:
    stc
    ret


; =============================================================================
; Assume 64MB fallback
; =============================================================================
assume_64mb:
    push es
    mov ax, BOOTINFO_SEG
    mov es, ax
    mov di, BI_E820_ENTRIES

    mov dword [es:di + 0], 0x00100000
    mov dword [es:di + 4], 0
    mov dword [es:di + 8], 0x03F00000
    mov dword [es:di + 12], 0
    mov dword [es:di + 16], 1
    mov dword [es:di + 20], 0

    mov dword [es:BI_E820_COUNT], 1
    pop es
    ret


; =============================================================================
; Compute max_phys_addr from E820 entries (32-bit max, sufficient for 4GB)
; =============================================================================
compute_max_phys_addr:
    push es
    mov ax, BOOTINFO_SEG
    mov es, ax

    mov ecx, [es:BI_E820_COUNT]
    test ecx, ecx
    jz .done

    mov si, BI_E820_ENTRIES
    xor edx, edx

.loop:
    mov eax, [es:si + 0]    ; base low
    add eax, [es:si + 8]    ; + length low
    cmp eax, edx
    jbe .next
    mov edx, eax
.next:
    add si, 24
    dec ecx
    jnz .loop

    mov [es:BI_MAX_PHYS_ADDR], edx

.done:
    pop es
    ret


; =============================================================================
; VBE Mode Setup
; Enumerate 32bpp linear framebuffer modes, select best, set it.
; =============================================================================
do_vbe:
    ; Get VBE Controller Info (0x4F00)
    ; ES:DI -> VBE info buffer at physical 0x11000 (segment 0x1100, offset 0)
    mov ax, VBE_INFO_SEG
    mov es, ax
    xor di, di
    mov dword [es:di], 0x32454256   ; 'VBE2'
    mov ax, 0x4F00
    int 0x10

    cmp ax, 0x004F
    jne .fail

    ; Check VBE version >= 2.0
    cmp word [es:4], 0x0200
    jb .fail

    ; Get mode list far pointer (at offset 14 of VbeInfoBlock)
    mov si, [es:14]          ; offset
    mov ax, [es:16]          ; segment
    mov fs, ax               ; FS:SI -> mode list

    ; Prepare to store modes in boot_info
    mov ax, BOOTINFO_SEG
    mov es, ax
    mov word [es:BI_VBE_MODE_COUNT], 0

    ; Init best mode tracking
    mov word [best_mode], 0xFFFF
    mov dword [best_score], 0

.enum_loop:
    mov cx, [fs:si]
    cmp cx, 0xFFFF
    je .enum_done
    add si, 2

    ; Get Mode Info (0x4F01) -> buffer at physical 0x11200
    push es
    push si
    push cx
    mov ax, VBE_MODE_SEG
    mov es, ax
    xor di, di
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    pop cx
    pop si
    jne .skip_mode_pop

    ; Check attributes: bit 0 = supported, bit 7 = linear FB
    mov al, [es:0]
    test al, 0x81
    jz .skip_mode_pop

    ; Check 32 bpp
    cmp byte [es:25], 32
    jne .skip_mode_pop

    ; Check memory model = direct color (6)
    cmp byte [es:27], 6
    jne .skip_mode_pop

    ; Save mode info locally before switching ES
    mov ax, [es:18]          ; width
    mov [tmp_width], ax
    mov ax, [es:20]          ; height
    mov [tmp_height], ax
    mov ax, [es:16]          ; pitch
    mov [tmp_pitch], ax
    mov eax, [es:40]         ; fb physical address
    mov [tmp_fb_addr], eax
    mov al, [es:25]          ; bpp
    mov [tmp_bpp], al

    ; Switch ES back to boot_info
    pop es                   ; restore ES = BOOTINFO_SEG

    ; Store in vbe_modes[] if room
    movzx eax, word [es:BI_VBE_MODE_COUNT]
    cmp eax, 32
    jge .skip_mode

    ; offset = BI_VBE_MODES + index * 16
    shl eax, 4
    add eax, BI_VBE_MODES
    mov di, ax

    mov ax, [tmp_width]
    mov [es:di + 0], ax
    mov ax, [tmp_height]
    mov [es:di + 2], ax
    mov ax, [tmp_pitch]
    mov [es:di + 4], ax
    mov [es:di + 6], cx         ; mode number
    mov eax, [tmp_fb_addr]
    mov [es:di + 8], eax
    mov al, [tmp_bpp]
    mov [es:di + 12], al
    mov byte [es:di + 13], 0
    mov byte [es:di + 14], 0
    mov byte [es:di + 15], 0

    inc word [es:BI_VBE_MODE_COUNT]

    ; Score: 1024x768=4, 800x600=3, 640x480=2, else=1
    push edx
    mov edx, 1
    cmp word [tmp_width], 640
    jne .not_640
    cmp word [tmp_height], 480
    jne .not_640
    mov edx, 2
.not_640:
    cmp word [tmp_width], 800
    jne .not_800
    cmp word [tmp_height], 600
    jne .not_800
    mov edx, 3
.not_800:
    cmp word [tmp_width], 1024
    jne .not_1024
    cmp word [tmp_height], 768
    jne .not_1024
    mov edx, 4
.not_1024:
    cmp edx, [best_score]
    jbe .no_new_best
    mov [best_score], edx
    mov [best_mode], cx
    mov eax, [tmp_fb_addr]
    mov [best_fb_addr], eax
    mov ax, [tmp_width]
    mov [best_width], ax
    mov ax, [tmp_height]
    mov [best_height], ax
    mov ax, [tmp_pitch]
    mov [best_pitch], ax
.no_new_best:
    pop edx

.skip_mode:
    jmp .enum_loop

.skip_mode_pop:
    pop es                   ; restore ES = BOOTINFO_SEG
    jmp .enum_loop

.enum_done:
    ; If no mode found, fail
    cmp word [best_mode], 0xFFFF
    je .fail

    ; Set the selected VBE mode with linear framebuffer (bit 14)
    mov bx, [best_mode]
    or bx, 0x4000
    mov ax, 0x4F02
    ; ES:DI is ignored for this call but some BIOSes want ES valid
    push ds
    pop es
    int 0x10
    cmp ax, 0x004F
    jne .fail

    ; Store selected mode info in boot_info
    mov ax, BOOTINFO_SEG
    mov es, ax

    mov eax, [best_fb_addr]
    mov [es:BI_FB_ADDR], eax
    movzx eax, word [best_width]
    mov [es:BI_FB_WIDTH], eax
    movzx eax, word [best_height]
    mov [es:BI_FB_HEIGHT], eax
    movzx eax, word [best_pitch]
    mov [es:BI_FB_PITCH], eax
    mov byte [es:BI_FB_BPP], 32
    mov ax, [best_mode]
    mov [es:BI_VBE_CURRENT_MODE], ax

    ; Restore ES
    xor ax, ax
    mov es, ax
    ret

.fail:
    ; fb_addr stays 0 — kernel uses VGA text mode
    xor ax, ax
    mov es, ax
    ret

; VBE temp data (below 64KB, in stage2 data area)
tmp_width:    dw 0
tmp_height:   dw 0
tmp_pitch:    dw 0
tmp_fb_addr:  dd 0
tmp_bpp:      db 0

best_mode:    dw 0xFFFF
best_score:   dd 0
best_fb_addr: dd 0
best_width:   dw 0
best_height:  dw 0
best_pitch:   dw 0


; =============================================================================
; Enter Unreal Mode
; =============================================================================
enter_unreal_mode:
    cli
    push ds
    push es

    lgdt [unreal_gdt_desc]

    mov eax, cr0
    or al, 1
    mov cr0, eax

    mov bx, 0x08
    mov ds, bx
    mov es, bx

    and al, 0xFE
    mov cr0, eax

    pop es
    pop ds
    sti
    ret

align 8
unreal_gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
unreal_gdt_end:

unreal_gdt_desc:
    dw unreal_gdt_end - unreal_gdt - 1
    dd unreal_gdt


; =============================================================================
; Load Kernel ELF
; Reads ELF from disk starting at sector 17, loads PT_LOAD segments to p_paddr
; above 1MB using unreal mode 32-bit addressing.
; =============================================================================
do_elf_load:
    ; Read ELF header (sector 17) into scratch at physical 0x20000
    ; Use segment 0x2000, offset 0
    mov dword [dap_lba], KERNEL_SECTOR_START
    mov word [dap_count], 1
    mov word [dap_offset], 0
    mov word [dap_segment], SCRATCH_SEG
    call disk_read

    ; Validate ELF magic at SCRATCH_SEG:0
    mov ax, SCRATCH_SEG
    mov gs, ax
    cmp dword [gs:0], ELF_MAGIC
    jne .bad_magic

    ; Validate: 32-bit, little-endian, executable
    cmp byte [gs:4], 1          ; EI_CLASS = ELFCLASS32
    jne .bad_format
    cmp byte [gs:5], 1          ; EI_DATA = ELFDATA2LSB
    jne .bad_format
    cmp word [gs:16], 2         ; e_type = ET_EXEC
    jne .bad_format

    ; Get entry point
    mov eax, [gs:24]            ; e_entry
    mov [elf_entry], eax

    ; Get program header info
    mov eax, [gs:28]            ; e_phoff
    mov [elf_phoff], eax
    movzx ecx, word [gs:44]    ; e_phnum
    mov [elf_phnum], cx
    movzx eax, word [gs:42]    ; e_phentsize
    mov [elf_phentsize], ax

    ; Store entry in boot_info
    push es
    mov ax, BOOTINFO_SEG
    mov es, ax
    mov eax, [elf_entry]
    mov [es:BI_KERNEL_ENTRY], eax
    mov dword [es:BI_KERNEL_PHYS_START], KERNEL_LOAD_ADDR
    mov dword [es:BI_KERNEL_PHYS_END], KERNEL_LOAD_ADDR
    mov dword [es:BI_KERNEL_LOADED_BYTES], 0
    mov dword [es:BI_KERNEL_SEGMENT_COUNT], 0
    pop es

    ; Read program headers into scratch buffer
    ; PHT starts at e_phoff bytes into the ELF file
    mov eax, [elf_phoff]
    shr eax, 9                  ; / 512 = sector offset within ELF
    add eax, KERNEL_SECTOR_START
    mov [dap_lba], eax

    ; Read 2 sectors to be safe
    mov word [dap_count], 2
    mov word [dap_offset], 0
    mov word [dap_segment], SCRATCH_SEG
    call disk_read

    ; PHT pointer = scratch_offset + (phoff % 512)
    mov eax, [elf_phoff]
    and eax, 0x1FF
    mov [phdr_off], ax          ; offset within the read buffer

    ; Process each program header
    movzx ecx, word [elf_phnum]

.phdr_loop:
    test ecx, ecx
    jz .done
    push ecx

    ; Read phdr fields from scratch via GS
    mov ax, SCRATCH_SEG
    mov gs, ax
    movzx esi, word [phdr_off]

    ; p_type
    cmp dword [gs:esi + 0], PT_LOAD
    jne .phdr_next

    ; Read segment fields
    mov eax, [gs:esi + 4]      ; p_offset
    mov [seg_offset], eax
    mov eax, [gs:esi + 12]     ; p_paddr
    mov [seg_paddr], eax
    mov eax, [gs:esi + 16]     ; p_filesz
    mov [seg_filesz], eax
    mov eax, [gs:esi + 20]     ; p_memsz
    mov [seg_memsz], eax

    ; Validate address range
    cmp dword [seg_paddr], KERNEL_LOAD_ADDR
    jb .bad_addr
    cmp dword [seg_paddr], KERNEL_MAX_ADDR
    jae .bad_addr

    ; Record segment in boot_info
    push es
    mov ax, BOOTINFO_SEG
    mov es, ax

    mov eax, [es:BI_KERNEL_SEGMENT_COUNT]
    cmp eax, 8
    jge .seg_skip

    shl eax, 3                  ; * 8 bytes per entry
    add eax, BI_KERNEL_SEGMENTS
    mov di, ax

    ; phys_start = page-aligned down
    mov edx, [seg_paddr]
    and edx, 0xFFFFF000
    mov [es:di + 0], edx

    ; phys_end = page-aligned up
    mov edx, [seg_paddr]
    add edx, [seg_memsz]
    add edx, 0xFFF
    and edx, 0xFFFFF000
    mov [es:di + 4], edx

    ; Update kernel_phys_end
    cmp edx, [es:BI_KERNEL_PHYS_END]
    jbe .no_end_update
    mov [es:BI_KERNEL_PHYS_END], edx
.no_end_update:

    ; Accumulate loaded bytes
    mov edx, [seg_filesz]
    add [es:BI_KERNEL_LOADED_BYTES], edx

    inc dword [es:BI_KERNEL_SEGMENT_COUNT]

.seg_skip:
    pop es

    ; --- Stream segment data from disk to target ---
    ; Starting LBA = KERNEL_SECTOR_START + (p_offset / 512)
    mov eax, [seg_offset]
    shr eax, 9
    add eax, KERNEL_SECTOR_START
    mov [cur_lba], eax

    ; Offset within first sector
    mov eax, [seg_offset]
    and eax, 0x1FF
    mov [cur_sect_off], eax

    mov dword [bytes_done], 0
    mov eax, [seg_paddr]
    mov [cur_dest], eax

.copy_loop:
    mov eax, [bytes_done]
    cmp eax, [seg_filesz]
    jge .bss_fill

    ; Read sectors into scratch buffer
    mov eax, [seg_filesz]
    sub eax, [bytes_done]
    add eax, [cur_sect_off]
    add eax, 511
    shr eax, 9
    ; Cap at 128 sectors (64KB, fits in scratch segment)
    cmp eax, 128
    jbe .cnt_ok
    mov eax, 128
.cnt_ok:
    mov [dap_count], ax
    mov eax, [cur_lba]
    mov [dap_lba], eax
    mov word [dap_offset], 0
    mov word [dap_segment], SCRATCH_SEG
    call disk_read

    ; Calculate bytes to copy this iteration
    movzx eax, word [dap_count]
    shl eax, 9                  ; sectors * 512
    sub eax, [cur_sect_off]     ; minus offset in first sector

    mov ecx, [seg_filesz]
    sub ecx, [bytes_done]       ; remaining
    cmp eax, ecx
    jbe .use_eax
    mov eax, ecx               ; don't copy more than remaining
.use_eax:
    mov ecx, eax               ; ECX = bytes to copy

    ; Source: physical 0x20000 + cur_sect_off
    mov esi, 0x20000
    add esi, [cur_sect_off]

    ; Dest: cur_dest (above 1MB, unreal mode)
    mov edi, [cur_dest]

    ; Copy using 32-bit addressing (unreal mode provides 4GB DS limit)
    call copy_high

    ; Update tracking
    add [bytes_done], ecx
    add [cur_dest], ecx

    ; Advance LBA
    movzx eax, word [dap_count]
    add [cur_lba], eax
    mov dword [cur_sect_off], 0

    jmp .copy_loop

.bss_fill:
    ; Zero BSS: memsz - filesz bytes
    mov ecx, [seg_memsz]
    sub ecx, [seg_filesz]
    jle .phdr_next

    mov edi, [cur_dest]
    call zero_high

.phdr_next:
    pop ecx
    ; Advance to next program header
    movzx eax, word [elf_phentsize]
    add [phdr_off], ax
    dec ecx
    jmp .phdr_loop

.done:
    ret

.bad_magic:
    call serial_print_msg
    db "ERROR: Bad ELF magic", 13, 10, 0
    jmp halt_boot

.bad_format:
    call serial_print_msg
    db "ERROR: Not 32-bit LE executable ELF", 13, 10, 0
    jmp halt_boot

.bad_addr:
    call serial_print_msg
    db "ERROR: Segment address out of range", 13, 10, 0
    jmp halt_boot


; =============================================================================
; Copy ECX bytes from [ESI] to [EDI] using unreal mode 32-bit addressing
; Both ESI and EDI can be > 1MB. DS descriptor cache has 4GB limit.
; =============================================================================
copy_high:
    pushad
    ; Use a32 prefix for 32-bit address operands in 16-bit code
.loop:
    test ecx, ecx
    jz .done
    a32 mov al, [ds:esi]
    a32 mov [ds:edi], al
    inc esi
    inc edi
    dec ecx
    jmp .loop
.done:
    popad
    ret


; =============================================================================
; Zero ECX bytes at [EDI] using unreal mode 32-bit addressing
; =============================================================================
zero_high:
    pushad
    xor al, al
.loop:
    test ecx, ecx
    jz .done
    a32 mov [ds:edi], al
    inc edi
    dec ecx
    jmp .loop
.done:
    popad
    ret


; =============================================================================
; Disk Read (INT 13h AH=42h with retry)
; =============================================================================
disk_read:
    mov byte [disk_retries], 3

.retry:
    mov si, dap_struct
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jnc .ok

    dec byte [disk_retries]
    jz .fail

    xor ah, ah
    mov dl, [boot_drive]
    int 0x13
    jmp .retry

.ok:
    ret

.fail:
    call serial_print_msg
    db "ERROR: Disk read failed", 13, 10, 0
    jmp halt_boot

; DAP structure
align 4
dap_struct:
    db 16               ; size
    db 0                ; reserved
dap_count:  dw 0
dap_offset: dw 0
dap_segment: dw 0
dap_lba:    dq 0

disk_retries: db 3


; =============================================================================
; Enter Protected Mode and Jump to Kernel
; =============================================================================
enter_protected_mode:
    cli

    ; Enable A20 (fast method)
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al

    ; Verify A20 by checking memory wrapping
    ; With A20 disabled, address 0x100500 wraps to 0x000500
    ; Use 0x000500 (free scratch area) to avoid corrupting kernel at 0x100000
    a32 mov byte [ds:0x000500], 0x00
    a32 mov byte [ds:0x100500], 0xFF
    a32 cmp byte [ds:0x000500], 0xFF
    jne .a20_ok

    ; Try keyboard controller method
    call a20_keyboard

    a32 mov byte [ds:0x000500], 0x00
    a32 mov byte [ds:0x100500], 0xFF
    a32 cmp byte [ds:0x000500], 0xFF
    jne .a20_ok

    call serial_print_msg
    db "WARNING: A20 may have failed", 13, 10, 0

.a20_ok:
    lgdt [pm_gdt_desc]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:pm_entry


; A20 via keyboard controller
a20_keyboard:
    call .wait_in
    mov al, 0xAD
    out 0x64, al
    call .wait_in
    mov al, 0xD0
    out 0x64, al
    call .wait_out
    in al, 0x60
    push ax
    call .wait_in
    mov al, 0xD1
    out 0x64, al
    call .wait_in
    pop ax
    or al, 2
    out 0x60, al
    call .wait_in
    mov al, 0xAE
    out 0x64, al
    call .wait_in
    ret
.wait_in:
    in al, 0x64
    test al, 2
    jnz .wait_in
    ret
.wait_out:
    in al, 0x64
    test al, 1
    jz .wait_out
    ret


halt_boot:
    cli
.halt:
    hlt
    jmp .halt


; =============================================================================
; 32-bit Protected Mode Entry
; =============================================================================
[BITS 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    cld

    mov esp, 0x90000
    xor ebp, ebp

    ; Push boot_info pointer (cdecl)
    push dword 0x10000

    ; Call kernel entry point
    mov eax, [0x10000 + BI_KERNEL_ENTRY]
    call eax

    cli
.hang:
    hlt
    jmp .hang


; =============================================================================
; Protected Mode GDT
; =============================================================================
[BITS 16]
align 8
pm_gdt:
    dq 0                    ; null
    ; 0x08: Code — base=0, limit=4GB, exec/read, 32-bit
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    ; 0x10: Data — base=0, limit=4GB, read/write, 32-bit
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
pm_gdt_end:

pm_gdt_desc:
    dw pm_gdt_end - pm_gdt - 1
    dd pm_gdt


; =============================================================================
; Data
; =============================================================================
boot_drive:     db 0

; ELF parsing
elf_entry:      dd 0
elf_phoff:      dd 0
elf_phnum:      dw 0
elf_phentsize:  dw 0
phdr_off:       dw 0

; Segment loading
seg_offset:     dd 0
seg_paddr:      dd 0
seg_filesz:     dd 0
seg_memsz:      dd 0
cur_lba:        dd 0
cur_sect_off:   dd 0
cur_dest:       dd 0
bytes_done:     dd 0
