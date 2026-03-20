; AIOS v2 — ISR assembly stubs (Phase 2)
; 32 CPU exception handler entry points

[BITS 32]

; External C handler
extern isr_common_handler

; Macro for exceptions that do NOT push an error code
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push dword 0        ; dummy error code
    push dword %1       ; interrupt number
    jmp isr_common_stub
%endmacro

; Macro for exceptions that DO push an error code
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push dword %1       ; interrupt number (error code already on stack)
    jmp isr_common_stub
%endmacro

; Generate all 32 exception stubs
ISR_NOERRCODE 0     ; Division by zero
ISR_NOERRCODE 1     ; Debug
ISR_NOERRCODE 2     ; NMI
ISR_NOERRCODE 3     ; Breakpoint
ISR_NOERRCODE 4     ; Overflow
ISR_NOERRCODE 5     ; Bound range exceeded
ISR_NOERRCODE 6     ; Invalid opcode
ISR_NOERRCODE 7     ; Device not available
ISR_ERRCODE   8     ; Double fault
ISR_NOERRCODE 9     ; Coprocessor segment overrun
ISR_ERRCODE   10    ; Invalid TSS
ISR_ERRCODE   11    ; Segment not present
ISR_ERRCODE   12    ; Stack-segment fault
ISR_ERRCODE   13    ; General protection fault
ISR_ERRCODE   14    ; Page fault
ISR_NOERRCODE 15    ; Reserved
ISR_NOERRCODE 16    ; x87 FP exception
ISR_ERRCODE   17    ; Alignment check
ISR_NOERRCODE 18    ; Machine check
ISR_NOERRCODE 19    ; SIMD FP exception
ISR_NOERRCODE 20    ; Virtualization exception
ISR_ERRCODE   21    ; Control protection exception
ISR_NOERRCODE 22    ; Reserved
ISR_NOERRCODE 23    ; Reserved
ISR_NOERRCODE 24    ; Reserved
ISR_NOERRCODE 25    ; Reserved
ISR_NOERRCODE 26    ; Reserved
ISR_NOERRCODE 27    ; Reserved
ISR_NOERRCODE 28    ; Reserved
ISR_ERRCODE   29    ; VMM communication exception
ISR_ERRCODE   30    ; Security exception
ISR_NOERRCODE 31    ; Reserved

; Common stub: save state, call C handler, restore state
isr_common_stub:
    pusha               ; push EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI

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
    call isr_common_handler
    add esp, 4          ; pop argument

    pop gs
    pop fs
    pop es
    pop ds

    popa                ; restore general registers
    add esp, 8          ; pop int_no and err_code
    iret
