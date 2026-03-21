; AIOS v2 — Minimal setjmp/longjmp for Lua error handling on i686.
; Saves/restores only callee-saved registers + stack + return address.

global setjmp
global longjmp

section .text

; int setjmp(jmp_buf env)
; env is pointer in [esp+4]
setjmp:
    mov eax, [esp+4]        ; eax = &jmp_buf
    mov [eax+0],  ebx
    mov [eax+4],  esi
    mov [eax+8],  edi
    mov [eax+12], ebp
    mov [eax+16], esp       ; save stack pointer
    mov ecx, [esp]          ; return address
    mov [eax+20], ecx
    xor eax, eax            ; return 0 (direct call)
    ret

; void longjmp(jmp_buf env, int val)
longjmp:
    mov edx, [esp+4]        ; edx = &jmp_buf
    mov eax, [esp+8]        ; eax = val
    test eax, eax
    jnz .nonzero
    inc eax                 ; longjmp(env, 0) returns 1, per POSIX
.nonzero:
    mov ebx, [edx+0]
    mov esi, [edx+4]
    mov edi, [edx+8]
    mov ebp, [edx+12]
    mov esp, [edx+16]       ; restore stack
    jmp [edx+20]            ; jump to saved return address
