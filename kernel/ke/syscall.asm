; LikeOS-64 SYSCALL/SYSRET Entry Point

BITS 64
SECTION .text

extern syscall_handler
extern g_current_kernel_stack    ; Per-task kernel stack pointer
global syscall_entry

SECTION .bss
align 16
kernel_syscall_stack:
    resb 8192
kernel_syscall_stack_top:

SECTION .data
user_rsp_save:
    dq 0

; Saved user context for fork
global syscall_saved_user_rip
global syscall_saved_user_rsp
global syscall_saved_user_rflags
syscall_saved_user_rip:
    dq 0
syscall_saved_user_rsp:
    dq 0
syscall_saved_user_rflags:
    dq 0

SECTION .text

syscall_entry:
    mov [rel user_rsp_save], rsp
    ; Use per-task kernel stack instead of global one
    mov rsp, [rel g_current_kernel_stack]
    
    push qword [rel user_rsp_save]
    push r11
    push rcx
    
    ; Save user context for fork() to use
    ; At this point: RCX = user RIP, R11 = user RFLAGS
    mov [rel syscall_saved_user_rip], rcx      ; RCX = user RIP
    mov [rel syscall_saved_user_rflags], r11   ; R11 = user RFLAGS
    push rax                                    ; Temporarily save syscall number
    mov rax, [rel user_rsp_save]
    mov [rel syscall_saved_user_rsp], rax      ; Save actual user RSP
    pop rax                                     ; Restore syscall number
    
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
    
    ; Restore stack to where registers were saved
    ; Use per-task kernel stack
    mov rsp, [rel g_current_kernel_stack]
    sub rsp, 16*8
    
    ; Store return value where rax will be popped from
    ; Stack layout (from rsp):
    ;   0: r15, 8: r14, 16: r13, 24: r12, 32: r10, 40: r9, 48: r8
    ;   56: rbp, 64: rdi, 72: rsi, 80: rdx, 88: rbx, 96: rax
    ;   104: rcx, 112: r11, 120: user_rsp
    mov [rsp + 12*8], rax
    
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

; Fork child return trampoline
; Stack has: RAX value, then IRET frame (RIP, CS, RFLAGS, RSP, SS)
global fork_child_return
fork_child_return:
    pop rax        ; Get fork return value (0)
    iretq          ; Return to userspace

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
