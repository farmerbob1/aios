; AIOS v2 — IRQ assembly stubs (Phase 2)
; 16 hardware interrupt entry points (IRQ 0-15 → INT 32-47)

[BITS 32]

extern irq_common_handler

; Macro: IRQ stub pushes dummy error code + interrupt number
%macro IRQ_STUB 2
global irq%1
irq%1:
    push dword 0        ; dummy error code
    push dword %2       ; interrupt number (32-47)
    jmp irq_common_stub
%endmacro

; Generate IRQ stubs: IRQ_STUB irq_num, int_num
IRQ_STUB 0,  32
IRQ_STUB 1,  33
IRQ_STUB 2,  34
IRQ_STUB 3,  35
IRQ_STUB 4,  36
IRQ_STUB 5,  37
IRQ_STUB 6,  38
IRQ_STUB 7,  39
IRQ_STUB 8,  40
IRQ_STUB 9,  41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

; Common stub: save state, call C handler, restore state
irq_common_stub:
    pusha

    push ds
    push es
    push fs
    push gs

    mov ax, 0x10        ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp            ; pointer to registers struct
    call irq_common_handler
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds

    popa
    add esp, 8          ; pop int_no and err_code
    iret
