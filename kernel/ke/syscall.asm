; LikeOS-64 SYSCALL/SYSRET Entry Point

BITS 64
SECTION .text

extern syscall_handler
global syscall_entry

; Per-CPU offsets into percpu_t (must match struct percpu in percpu.h)
; These fields are right after the self-pointer at GS:0.
%define PERCPU_SYSCALL_KRSP         8    ; percpu_t::syscall_kernel_rsp
%define PERCPU_SYSCALL_URSP         16   ; percpu_t::syscall_user_rsp
%define PERCPU_SAVED_USER_RIP       24   ; percpu_t::syscall_saved_user_rip
%define PERCPU_SAVED_USER_RFLAGS    32   ; percpu_t::syscall_saved_user_rflags
%define PERCPU_SAVED_USER_RBP       40   ; percpu_t::syscall_saved_user_rbp
%define PERCPU_SAVED_USER_RBX       48   ; percpu_t::syscall_saved_user_rbx
%define PERCPU_SAVED_USER_R12       56   ; percpu_t::syscall_saved_user_r12
%define PERCPU_SAVED_USER_R13       64   ; percpu_t::syscall_saved_user_r13
%define PERCPU_SAVED_USER_R14       72   ; percpu_t::syscall_saved_user_r14
%define PERCPU_SAVED_USER_R15       80   ; percpu_t::syscall_saved_user_r15
%define PERCPU_SAVED_USER_RAX       88   ; percpu_t::syscall_saved_user_rax
%define PERCPU_SIGNAL_PENDING       96   ; percpu_t::syscall_signal_pending

SECTION .bss
align 16
kernel_syscall_stack:
    resb 8192
kernel_syscall_stack_top:

SECTION .data
; Saved kernel RSP before syscall_handler call (for restoring after context switch)
syscall_saved_ksp:
    dq 0

; NOTE: The following global variables are DEPRECATED and kept only for backward
; compatibility with C code that hasn't been updated yet. The assembly code now
; uses per-CPU storage via GS: prefix for SMP safety.
global syscall_saved_user_rip
global syscall_saved_user_rsp
global syscall_saved_user_rflags
global syscall_saved_user_rbp
global syscall_saved_user_rbx
global syscall_saved_user_r12
global syscall_saved_user_r13
global syscall_saved_user_r14
global syscall_saved_user_r15
global syscall_saved_user_rax
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
syscall_saved_user_rax:
    dq 0

; Signal delivery: per-CPU via GS:PERCPU_SIGNAL_PENDING (global kept for compat)
global syscall_signal_pending
syscall_signal_pending:
    dq 0

SECTION .text

syscall_entry:
    mov [gs:PERCPU_SYSCALL_URSP], rsp
    ; Use per-CPU kernel stack (set by context switch on this CPU)
    mov rsp, [gs:PERCPU_SYSCALL_KRSP]
    
    push qword [gs:PERCPU_SYSCALL_URSP]
    push r11
    push rcx
    
    ; Save user context to PER-CPU storage for fork() and signal handling
    ; At this point: RCX = user RIP, R11 = user RFLAGS
    ; Using GS-relative addressing for SMP safety (each CPU has its own copy)
    mov [gs:PERCPU_SAVED_USER_RIP], rcx        ; RCX = user RIP
    mov [gs:PERCPU_SAVED_USER_RFLAGS], r11     ; R11 = user RFLAGS
    push rax                                    ; Temporarily save syscall number
    mov rax, [gs:PERCPU_SYSCALL_URSP]
    ; Note: user RSP is already in PERCPU_SYSCALL_URSP, no need to duplicate
    pop rax                                     ; Restore syscall number
    
    ; Save callee-saved registers to per-CPU storage for fork()
    mov [gs:PERCPU_SAVED_USER_RBP], rbp
    mov [gs:PERCPU_SAVED_USER_RBX], rbx
    mov [gs:PERCPU_SAVED_USER_R12], r12
    mov [gs:PERCPU_SAVED_USER_R13], r13
    mov [gs:PERCPU_SAVED_USER_R14], r14
    mov [gs:PERCPU_SAVED_USER_R15], r15
    
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
    
    ; Use RBP as frame pointer to remember where our saved registers are
    ; This way we can find them even after sched_yield context switches
    mov rbp, rsp                               ; RBP = top of our saved register area
    
    and rsp, ~0xF
    ; NOTE: Interrupts remain DISABLED here. syscall_handler will enable them
    ; AFTER copying per-CPU values to task-local storage (to prevent race condition
    ; where another task overwrites our per-CPU data before we read it).
    call syscall_handler
    cli                                        ; Disable interrupts for return path
    
    ; Check if signal delivery modified the return context
    ; If per-CPU syscall_signal_pending is non-zero, use modified context for signal handler
    mov rdi, [gs:PERCPU_SIGNAL_PENDING]
    test rdi, rdi
    jnz .signal_return
    
    ; Normal syscall return path
    ; Restore RSP from RBP (which was preserved through any context switches
    ; since it's a callee-saved register)
    mov rsp, rbp
    
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
    ; RDI has value from per-CPU syscall_signal_pending
    ; If RDI == -1 (0xFFFFFFFFFFFFFFFF), this is sigreturn - don't set RDI to signal
    ; Otherwise, RDI is the signal number for the handler
    
    ; Clear the per-CPU pending flag
    xor rax, rax
    mov [gs:PERCPU_SIGNAL_PENDING], rax
    
    ; Check if this is sigreturn (RDI == -1) or signal delivery
    cmp rdi, -1
    je .sigreturn_restore
    
    ; Signal handler call - RDI already has signal number
    ; Load modified context from per-CPU saved variables
    mov rcx, [gs:PERCPU_SAVED_USER_RIP]       ; Handler address -> RCX for SYSRET
    mov r11, [gs:PERCPU_SAVED_USER_RFLAGS]    ; RFLAGS -> R11 for SYSRET
    mov rsp, [gs:PERCPU_SYSCALL_URSP]         ; Signal frame on stack (stored in user_rsp)
    
    ; Restore callee-saved registers (handler expects these)
    mov rbp, [gs:PERCPU_SAVED_USER_RBP]
    mov rbx, [gs:PERCPU_SAVED_USER_RBX]
    mov r12, [gs:PERCPU_SAVED_USER_R12]
    mov r13, [gs:PERCPU_SAVED_USER_R13]
    mov r14, [gs:PERCPU_SAVED_USER_R14]
    mov r15, [gs:PERCPU_SAVED_USER_R15]
    
    ; Clear other registers for security (except RDI which has signal number)
    xor rax, rax
    xor rsi, rsi
    xor rdx, rdx
    xor r8, r8
    xor r9, r9
    xor r10, r10
    
    o64 sysret

.sigreturn_restore:
    ; Sigreturn path - restore full context from per-CPU saved variables
    ; The saved variables were already updated by signal_restore_frame
    mov rcx, [gs:PERCPU_SAVED_USER_RIP]       ; Original RIP -> RCX for SYSRET
    mov r11, [gs:PERCPU_SAVED_USER_RFLAGS]    ; RFLAGS -> R11 for SYSRET
    mov rsp, [gs:PERCPU_SYSCALL_URSP]         ; Original user RSP
    
    ; Restore callee-saved registers
    mov rbp, [gs:PERCPU_SAVED_USER_RBP]
    mov rbx, [gs:PERCPU_SAVED_USER_RBX]
    mov r12, [gs:PERCPU_SAVED_USER_R12]
    mov r13, [gs:PERCPU_SAVED_USER_R13]
    mov r14, [gs:PERCPU_SAVED_USER_R14]
    mov r15, [gs:PERCPU_SAVED_USER_R15]
    
    ; Restore RAX (syscall return value, e.g., -EINTR)
    mov rax, [gs:PERCPU_SAVED_USER_RAX]
    
    ; Clear other registers
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
