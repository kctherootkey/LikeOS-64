; LikeOS-64 SYSCALL/SYSRET Entry Point

BITS 64
SECTION .text

extern syscall_handler
global syscall_entry

SECTION .bss
align 16
kernel_syscall_stack:
    resb 8192
kernel_syscall_stack_top:

SECTION .data
user_rsp_save:
    dq 0

SECTION .text

syscall_entry:
    mov [rel user_rsp_save], rsp
    lea rsp, [rel kernel_syscall_stack_top]
    
    push qword [rel user_rsp_save]
    push r11
    push rcx
    
    push rax
    push rbx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r12
    push r13
    push r14
    push r15
    
    mov r9, r8
    mov r8, r10
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax
    
    and rsp, ~0xF
    sti
    call syscall_handler
    
    lea rsp, [rel kernel_syscall_stack_top]
    sub rsp, 16*8
    mov [rsp + 14*8], rax
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rbx
    pop rax
    
    pop rcx
    pop r11
    pop rsp
    
    o64 sysret

; IRET trampoline for user mode entry
global user_mode_iret_trampoline
user_mode_iret_trampoline:
    iretq

; Context switch between tasks
global ctx_switch_asm
ctx_switch_asm:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    mov [rdi], rsp
    mov rsp, rsi
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    
    ret
