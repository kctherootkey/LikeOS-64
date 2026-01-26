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
    
; Saved kernel RSP before syscall_handler call (for restoring after context switch)
syscall_saved_ksp:
    dq 0

; Saved user context for fork
global syscall_saved_user_rip
global syscall_saved_user_rsp
global syscall_saved_user_rflags
global syscall_saved_user_rbp
global syscall_saved_user_rbx
global syscall_saved_user_r12
global syscall_saved_user_r13
global syscall_saved_user_r14
global syscall_saved_user_r15
syscall_saved_user_rip:
    dq 0
syscall_saved_user_rsp:
    dq 0
syscall_saved_user_rflags:
    dq 0
syscall_saved_user_rbp:
    dq 0
syscall_saved_user_rbx:
    dq 0
syscall_saved_user_r12:
    dq 0
syscall_saved_user_r13:
    dq 0
syscall_saved_user_r14:
    dq 0
syscall_saved_user_r15:
    dq 0

; Signal delivery: if non-zero, this is the signal number to pass in RDI
global syscall_signal_pending
syscall_signal_pending:
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
    
    ; Save callee-saved registers for fork()
    mov [rel syscall_saved_user_rbp], rbp
    mov [rel syscall_saved_user_rbx], rbx
    mov [rel syscall_saved_user_r12], r12
    mov [rel syscall_saved_user_r13], r13
    mov [rel syscall_saved_user_r14], r14
    mov [rel syscall_saved_user_r15], r15
    
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
    
    ; Check if signal delivery modified the return context
    ; If syscall_signal_pending is non-zero, use modified context for signal handler
    mov rdi, [rel syscall_signal_pending]
    test rdi, rdi
    jnz .signal_return
    
    ; Normal syscall return path
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

.signal_return:
    ; Signal handler or sigreturn path
    ; RDI has value from syscall_signal_pending
    ; If RDI == -1 (0xFFFFFFFFFFFFFFFF), this is sigreturn - don't set RDI to signal
    ; Otherwise, RDI is the signal number for the handler
    
    ; Clear the pending flag
    xor rax, rax
    mov [rel syscall_signal_pending], rax
    
    ; Check if this is sigreturn (RDI == -1) or signal delivery
    cmp rdi, -1
    je .sigreturn_restore
    
    ; Signal handler call - RDI already has signal number
    ; Load modified context from saved variables
    mov rcx, [rel syscall_saved_user_rip]     ; Handler address -> RCX for SYSRET
    mov r11, [rel syscall_saved_user_rflags]  ; RFLAGS -> R11 for SYSRET
    mov rsp, [rel syscall_saved_user_rsp]     ; Signal frame on stack
    
    ; Restore callee-saved registers (handler expects these)
    mov rbp, [rel syscall_saved_user_rbp]
    mov rbx, [rel syscall_saved_user_rbx]
    mov r12, [rel syscall_saved_user_r12]
    mov r13, [rel syscall_saved_user_r13]
    mov r14, [rel syscall_saved_user_r14]
    mov r15, [rel syscall_saved_user_r15]
    
    ; Clear other registers for security (except RDI which has signal number)
    xor rax, rax
    xor rsi, rsi
    xor rdx, rdx
    xor r8, r8
    xor r9, r9
    xor r10, r10
    
    o64 sysret

.sigreturn_restore:
    ; Sigreturn path - restore full context from saved variables
    ; The saved variables were already updated by signal_restore_frame
    mov rcx, [rel syscall_saved_user_rip]     ; Original RIP -> RCX for SYSRET
    mov r11, [rel syscall_saved_user_rflags]  ; RFLAGS -> R11 for SYSRET
    mov rsp, [rel syscall_saved_user_rsp]     ; Original user RSP
    
    ; Restore callee-saved registers
    mov rbp, [rel syscall_saved_user_rbp]
    mov rbx, [rel syscall_saved_user_rbx]
    mov r12, [rel syscall_saved_user_r12]
    mov r13, [rel syscall_saved_user_r13]
    mov r14, [rel syscall_saved_user_r14]
    mov r15, [rel syscall_saved_user_r15]
    
    ; Clear other registers
    xor rax, rax
    xor rdi, rdi
    xor rsi, rsi
    xor rdx, rdx
    xor r8, r8
    xor r9, r9
    xor r10, r10
    
    o64 sysret

; IRET trampoline for user mode entry
global user_mode_iret_trampoline
user_mode_iret_trampoline:
    iretq

; Fork child return trampoline
; Stack layout when we get here (from RSP upward):
;   RAX value (0)
;   IRET frame: RIP, CS, RFLAGS, RSP, SS
;   User callee-saved: RBP, RBX, R12, R13, R14, R15
; We need to pop RAX, then load the callee-saved regs (after IRET frame), then iretq
global fork_child_return
fork_child_return:
    pop rax        ; Get fork return value (0)
    
    ; IRET frame starts at RSP (5 qwords: RIP, CS, RFLAGS, RSP, SS)
    ; User callee-saved regs are at RSP + 40 (after the 5 qwords)
    ; Order on stack: RBP, RBX, R12, R13, R14, R15 (pushed in reverse, so RBP is first)
    mov rbp, [rsp + 40]
    mov rbx, [rsp + 48]
    mov r12, [rsp + 56]
    mov r13, [rsp + 64]
    mov r14, [rsp + 72]
    mov r15, [rsp + 80]
    
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
