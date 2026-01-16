; LikeOS-64 SYSCALL Entry Point
; This handles transitions from user mode (ring 3) to kernel mode (ring 0)
;
; On SYSCALL entry (from CPU):
;   RCX = return RIP (user's next instruction)
;   R11 = return RFLAGS
;   RSP = still user stack! (SYSCALL does NOT switch stacks)
;
; System call convention (Linux style):
;   RAX = syscall number
;   RDI, RSI, RDX, R10, R8, R9 = arguments 1-6
;   RAX = return value
;
; Note: RCX and R11 are clobbered by SYSCALL, so R10 replaces RCX for arg4

BITS 64
SECTION .text

; External C handler
extern syscall_handler

; Export syscall_entry for use by MmInitializeSyscall
global syscall_entry

; Per-CPU kernel stack (single CPU for now)
SECTION .bss
align 16
kernel_syscall_stack:
    resb 8192
kernel_syscall_stack_top:

SECTION .data
user_rsp_save:
    dq 0

SECTION .text

; Syscall entry point - called via SYSCALL instruction from user mode
syscall_entry:
    ; Immediately switch to kernel stack (user RSP is not trusted)
    mov [rel user_rsp_save], rsp
    lea rsp, [rel kernel_syscall_stack_top]
    
    ; Build stack frame for SYSRET return
    ; Stack layout (top to bottom):
    ;   [rsp+16] = user RSP
    ;   [rsp+8]  = user RFLAGS (R11)
    ;   [rsp+0]  = user RIP (RCX)
    push qword [rel user_rsp_save]  ; User RSP
    push r11                         ; User RFLAGS
    push rcx                         ; User RIP
    
    ; Save all general purpose registers (callee-saved and others)
    push rax        ; Syscall number
    push rbx
    push rcx        ; Already saved RIP, but save again for consistency
    push rdx        ; arg3
    push rsi        ; arg2
    push rdi        ; arg1
    push rbp
    push r8         ; arg5
    push r9         ; arg6
    push r10        ; arg4
    push r11        ; Already saved RFLAGS
    push r12
    push r13
    push r14
    push r15
    
    ; Set up C calling convention for syscall_handler
    ; int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2,
    ;                         uint64_t a3, uint64_t a4, uint64_t a5)
    ; Mapping: rdi=num, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5
    ; From:    rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5
    
    ; We need to shuffle registers carefully
    mov r9, r8              ; a5: r8 -> r9
    mov r8, r10             ; a4: r10 -> r8
    mov rcx, rdx            ; a3: rdx -> rcx
    mov rdx, rsi            ; a2: rsi -> rdx
    mov rsi, rdi            ; a1: rdi -> rsi
    mov rdi, rax            ; num: rax -> rdi
    
    ; Ensure 16-byte stack alignment before call
    and rsp, ~0xF
    
    ; Call C syscall handler
    call syscall_handler
    
    ; RAX contains return value
    ; Restore stack pointer to where registers are saved
    lea rsp, [rel kernel_syscall_stack_top]
    sub rsp, 18*8           ; Point to saved r15
    
    ; Store return value where RAX was saved
    mov [rsp + 14*8], rax   ; Overwrite saved RAX
    
    ; Restore all general purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax                 ; Return value
    
    ; Skip the duplicate RCX we pushed
    add rsp, 8
    
    ; Restore user state for SYSRET
    pop rcx                 ; User RIP -> RCX
    pop r11                 ; User RFLAGS -> R11
    pop rsp                 ; User RSP
    
    ; Return to user mode
    ; SYSRET loads: RIP from RCX, RFLAGS from R11
    ; CS = STAR[63:48] + 16, SS = STAR[63:48] + 8 (both ORed with 3 for ring 3)
    o64 sysret
