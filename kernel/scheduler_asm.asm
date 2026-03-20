; AIOS v2 — Context switch assembly (Phase 2)

[BITS 32]

; void task_switch(uint32_t* old_esp, uint32_t new_esp)
global task_switch
task_switch:
    push ebp
    push ebx
    push esi
    push edi

    mov eax, [esp+20]      ; &old_esp (4 pushes + ret addr = 20)
    mov [eax], esp          ; save current ESP

    mov esp, [esp+24]       ; load new ESP (read from OLD stack before switch)

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                     ; returns to new task's saved EIP
