; AIOS UEFI Bootloader — 64-bit to 32-bit Mode Transition
;
; Called after ExitBootServices() from the C bootloader.
; Transitions from x86_64 long mode to i686 32-bit protected mode,
; then jumps to kernel_main(boot_info*).
;
; Entry (MS x64 ABI):
;   RCX = kernel_entry (32-bit physical address)
;   RDX = boot_info_ptr (32-bit physical address, 0x10000)
;
; Exit state (matches old BIOS bootloader contract):
;   - 32-bit protected mode (CR0.PE=1, CR0.PG=0)
;   - Long mode disabled (EFER.LME=0)
;   - CS=0x08 (32-bit code), DS/ES/FS/GS/SS=0x10 (32-bit data)
;   - ESP=0x90000
;   - [ESP+4] = boot_info pointer (cdecl)
;   - EFLAGS.IF=0 (interrupts disabled)
;
; Assembled with: nasm -f win64 transition.asm -o transition.o

    bits 64
    default rel
    section .text

    global enter_32bit_and_jump

; Macro: output a single char to COM1 0x3F8 (works in any CPU mode)
%macro SERIAL_CHAR 1
    push    eax
    push    edx
%%wait:
    mov     dx, 0x3FD
    in      al, dx
    test    al, 0x20
    jz      %%wait
    mov     dx, 0x3F8
    mov     al, %1
    out     dx, al
    pop     edx
    pop     eax
%endmacro

enter_32bit_and_jump:
    ; Save parameters before we lose 64-bit registers
    ; RCX = kernel_entry, RDX = boot_info_ptr
    mov     esi, ecx        ; ESI = kernel entry (32-bit)
    mov     edi, edx        ; EDI = boot_info ptr (32-bit)

    ; Disable interrupts (should already be disabled post-ExitBootServices)
    cli

    ; ── Step 1: Load GDT with 32-bit segments ──────────
    lea     rax, [gdt_desc]
    lgdt    [rax]

    ; ── Step 2: Far return to compatibility mode ───────
    ; Long mode doesn't have a direct far jump instruction.
    ; We use retfq: push segment selector + target address, then retfq.
    lea     rax, [compat_entry]
    push    0x08            ; 32-bit code segment selector
    push    rax             ; target address
    retfq                   ; far return → loads CS=0x08, RIP=compat_entry

    ; ── Now in 32-bit compatibility mode ───────────────
    bits 32

compat_entry:
    SERIAL_CHAR '1'         ; Reached compatibility mode

    ; Load 32-bit data segments immediately
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    SERIAL_CHAR '2'         ; Segments loaded

    ; ── Step 3: Disable paging ─────────────────────────
    ; This also effectively exits long mode (PG=0 means no translation)
    mov     eax, cr0
    and     eax, ~(1 << 31) ; Clear CR0.PG
    mov     cr0, eax

    SERIAL_CHAR '3'         ; Paging disabled

    ; ── Step 3b: Clear CR4 (PAE, PGE, etc.) ──────────
    ; CRITICAL: UEFI long mode sets CR4.PAE=1. If we leave it set,
    ; the kernel's CR0.PG=1 would activate PAE paging (3-level, 64-bit
    ; entries) instead of classic 32-bit paging. Triple fault!
    xor     eax, eax
    mov     cr4, eax

    SERIAL_CHAR '4'         ; CR4 cleared

    ; ── Step 4: Disable long mode in EFER MSR ──────────
    mov     ecx, 0xC0000080 ; IA32_EFER MSR
    rdmsr
    and     eax, ~(1 << 8)  ; Clear LME (Long Mode Enable)
    wrmsr

    SERIAL_CHAR '5'         ; EFER.LME cleared

    ; ── Step 5: Flush pipeline with a near relative jump ─
    ; CS is already 0x08 from the retfq. Near jump avoids
    ; absolute address relocations that PE32+ can't handle.
    jmp     .pm32

.pm32:
    SERIAL_CHAR '6'         ; In pure 32-bit mode

    ; ── Step 6: Reload data segments (safety) ──────────
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    ; ── Step 7: Set up kernel stack ────────────────────
    mov     esp, 0x90000

    SERIAL_CHAR '7'         ; About to jump to kernel

    ; ── Step 8: Call kernel_main(boot_info*) ───────────
    ; cdecl calling convention: argument on stack
    push    edi             ; boot_info pointer (0x10000)
    call    esi             ; kernel_main(boot_info*)

    ; kernel_main should never return, but halt if it does
    cli
    hlt
    jmp     $

    ; ════════════════════════════════════════════════════
    ; GDT for 32-bit protected mode
    ;
    ; 3 entries:
    ;   0x00: Null descriptor
    ;   0x08: 32-bit code segment (base=0, limit=4GB, DPL=0, exec/read)
    ;   0x10: 32-bit data segment (base=0, limit=4GB, DPL=0, read/write)
    ; ════════════════════════════════════════════════════

    align 16
gdt_start:
    ; Null descriptor
    dq 0x0000000000000000

    ; 32-bit code segment: base=0, limit=0xFFFFF (4KB granularity = 4GB)
    ; Access: present=1, DPL=0, type=code exec/read (0x9A)
    ; Flags: granularity=1, 32-bit=1, 64-bit=0 (0xCF)
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 0x9A         ; Access: P=1 DPL=00 S=1 Type=1010 (code exec/read)
    db 0xCF         ; Flags: G=1 D=1 L=0 + Limit 19:16=0xF
    db 0x00         ; Base 31:24

    ; 32-bit data segment: same as code but type=data read/write (0x92)
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 0x92         ; Access: P=1 DPL=00 S=1 Type=0010 (data read/write)
    db 0xCF         ; Flags: G=1 D=1 L=0 + Limit 19:16=0xF
    db 0x00         ; Base 31:24
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1     ; GDT limit (size - 1)
    dq gdt_start                    ; GDT base address (64-bit for lgdt in long mode)
