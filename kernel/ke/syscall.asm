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
    push rdx        ; arg3
    push rsi        ; arg2
    push rdi        ; arg1
    push rbp
    push r8         ; arg5
    push r9         ; arg6
    push r10        ; arg4
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

    ; Allow IRQs while running in kernel during syscall handling
    sti
    
    ; Call C syscall handler
    call syscall_handler
    
    ; RAX contains return value
    ; Restore stack pointer to where registers are saved
    lea rsp, [rel kernel_syscall_stack_top]
    sub rsp, 16*8           ; Point to saved r15 (16 registers pushed)
    
    ; Store return value where RAX was saved
    mov [rsp + 14*8], rax   ; Overwrite saved RAX
    
    ; Restore all general purpose registers
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
    pop rax                 ; Return value
    
    ; Restore user state for SYSRET
    pop rcx                 ; User RIP -> RCX
    pop r11                 ; User RFLAGS -> R11
    pop rsp                 ; User RSP
    
    ; Return to user mode
    ; SYSRET loads: RIP from RCX, RFLAGS from R11
    ; CS = STAR[63:48] + 16, SS = STAR[63:48] + 8 (both ORed with 3 for ring 3)
    o64 sysret

; user_mode_iret_trampoline - jump here with IRET frame on stack
; Stack should contain: RIP, CS, RFLAGS, RSP, SS (from low to high address)
global user_mode_iret_trampoline
user_mode_iret_trampoline:
    ; DEBUG: Output RSP to serial port (0x3F8)
    ; Format: Write 'RSP=' then 16 hex digits
    push rax
    push rdx
    push rcx
    push rbx
    
    ; Output "RSP="
    mov dx, 0x3F8
    mov al, 'R'
    out dx, al
    mov al, 'S'
    out dx, al
    mov al, 'P'
    out dx, al
    mov al, '='
    out dx, al
    
    ; Get RSP value (before our pushes, so add 32)
    lea rbx, [rsp + 32]
    
    ; Output 16 hex digits (from high to low nibble)
    mov rcx, 16
.hex_loop:
    rol rbx, 4          ; Rotate left 4 bits
    mov al, bl
    and al, 0x0F        ; Get low nibble
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .out
.digit:
    add al, '0'
.out:
    out dx, al
    dec rcx
    jnz .hex_loop
    
    ; Newline
    mov al, 13
    out dx, al
    mov al, 10
    out dx, al
    
    ; Now dump the IRET frame
    ; Output "[rsp+0]=" and the RIP value
    lea rbx, [rsp + 32]  ; RSP after restoring our pushes
    
    mov al, '['
    out dx, al
    mov al, '0'
    out dx, al
    mov al, ']'
    out dx, al
    mov al, '='
    out dx, al
    
    mov rbx, [rsp + 32]   ; RIP value
    mov rcx, 16
.hex_loop2:
    rol rbx, 4
    mov al, bl
    and al, 0x0F
    cmp al, 10
    jb .digit2
    add al, 'A' - 10
    jmp .out2
.digit2:
    add al, '0'
.out2:
    out dx, al
    dec rcx
    jnz .hex_loop2
    
    mov al, 13
    out dx, al
    mov al, 10
    out dx, al
    
    pop rbx
    pop rcx
    pop rdx
    pop rax
    
    ; DEBUG: Print all 5 IRET frame values
    push rax
    push rdx
    push rbx
    push rcx
    
    mov dx, 0x3F8
    
    ; Print each value
    %macro PRINT_SLOT 2
        mov al, %1
        out dx, al
        mov al, '='
        out dx, al
        mov rbx, [rsp + 32 + %2*8]
        mov rcx, 16
    %%hex:
        rol rbx, 4
        mov al, bl
        and al, 0x0F
        cmp al, 10
        jb %%d
        add al, 'A' - 10
        jmp %%o
    %%d:
        add al, '0'
    %%o:
        out dx, al
        dec rcx
        jnz %%hex
        mov al, ' '
        out dx, al
    %endmacro
    
    PRINT_SLOT 'R', 0   ; RIP
    PRINT_SLOT 'C', 1   ; CS
    PRINT_SLOT 'F', 2   ; RFLAGS
    PRINT_SLOT 'S', 3   ; RSP
    PRINT_SLOT 'X', 4   ; SS
    
    mov al, 13
    out dx, al
    mov al, 10
    out dx, al
    
    pop rcx
    pop rbx
    pop rdx
    pop rax
    
    ; Verify stack contents before IRET
    ; Expected: [RSP+0]=0x400000, [RSP+8]=0x23, [RSP+16]=0x202
    mov rax, [rsp]        ; Load RIP value
    cmp rax, 0x400000
    jne .bad_rip
    
    mov rax, [rsp+8]      ; Load CS value
    cmp rax, 0x23
    jne .bad_cs
    
    mov rax, [rsp+16]     ; Load RFLAGS value
    cmp rax, 0x202
    jne .bad_rflags
    
    ; Everything looks good, do IRET
    cli
    iretq

.bad_rip:
    ; RIP is wrong - halt for debugging
    ; RAX contains actual value, can see in QEMU
    mov rbx, 0xBAD1
    hlt
    jmp $
    
.bad_cs:
    mov rbx, 0xBAD2
    hlt
    jmp $
    
.bad_rflags:
    mov rbx, 0xBAD3
    hlt
    jmp $

; ctx_switch_asm - context switch between tasks
; Arguments: rdi = pointer to old_sp (where to save current RSP)
;            rsi = new_sp (the RSP value to switch to)
global ctx_switch_asm
ctx_switch_asm:
    ; Save callee-saved registers on current stack
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    
    ; Save current RSP to *old_sp
    mov [rdi], rsp
    
    ; Load new stack pointer
    mov rsp, rsi
    
    ; Restore callee-saved registers from new stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    
    ; Return (will pop return address from new stack)
    ret