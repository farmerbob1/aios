; =============================================================================
; AIOS v2 — Stage 1 Bootloader (MBR, 512 bytes)
;
; Job: Load Stage 2 from disk sectors 1-16 into memory at 0x7E00, jump to it.
; Uses INT 13h extensions (LBA mode, AH=0x42) — simpler than CHS, works for
; any disk size.
;
; DESIGN CONTRACT: Stage 2 is always sectors 1-16 (8KB max).
; =============================================================================

[BITS 16]
[ORG 0x7C00]

start:
    ; Clear interrupts during setup
    cli

    ; Set up segments — all zero for flat real mode addressing
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; Stack grows down below Stage 1

    ; Re-enable interrupts (needed for BIOS calls)
    sti

    ; Save boot drive number (BIOS passes it in DL)
    mov [boot_drive], dl

    ; --- Load Stage 2: 16 sectors from LBA 1 to 0x7E00 ---
    mov byte [retries], 3

.retry_loop:
    ; Set up DAP (Disk Address Packet) for INT 13h AH=42h
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jnc .load_ok            ; CF clear = success

    ; Read failed — retry with disk reset
    dec byte [retries]
    jz .fail

    ; Reset disk (INT 13h AH=00h)
    xor ah, ah
    mov dl, [boot_drive]
    int 0x13
    jmp .retry_loop

.load_ok:
    ; Pass boot drive to Stage 2 in DL
    mov dl, [boot_drive]

    ; Jump to Stage 2
    jmp 0x0000:0x7E00

.fail:
    ; Print 'E' for error and halt
    mov ah, 0x0E
    mov al, 'E'
    xor bh, bh
    int 0x10
    cli
.hang:
    hlt
    jmp .hang

; =============================================================================
; Data
; =============================================================================

boot_drive: db 0
retries:    db 3

; Disk Address Packet (DAP) — must be 16 bytes
; See: https://en.wikipedia.org/wiki/INT_13H#INT_13h_AH=42h:_Extended_Read_Sectors_From_Drive
align 4
dap:
    db 16               ; DAP size (16 bytes)
    db 0                ; reserved, must be 0
    dw 16               ; number of sectors to read (16 = 8KB)
    dw 0x7E00           ; destination offset
    dw 0x0000           ; destination segment
    dq 1                ; starting LBA (sector 1)

; =============================================================================
; Pad to 510 bytes and add boot signature
; =============================================================================
times 510 - ($ - $$) db 0
dw 0xAA55
