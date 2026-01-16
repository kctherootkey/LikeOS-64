; LikeOS-64 User Mode Test Program
; This is a simple user-mode program that tests syscalls
;
; Assembled to raw binary and copied to user address space

BITS 64
SECTION .text

; System call numbers
SYS_EXIT    equ 0
SYS_WRITE   equ 1
SYS_YIELD   equ 3
SYS_GETPID  equ 4

; Start of user program - this gets copied to user space
global user_test_start
global user_test_end

user_test_start:
    ; Get our PID
    mov rax, SYS_GETPID
    syscall
    mov r12, rax
    
    ; Write "Hello from user mode!" to stdout
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    ; Loop a few times, yielding each time
    mov r13, 3
.loop:
    mov rax, SYS_YIELD
    syscall
    dec r13
    jnz .loop

    ; Exit
    mov rax, SYS_EXIT
    mov rdi, 0
    syscall

.hang:
    jmp .hang

msg: db "[USER] Hello from user mode! Task running in Ring 3", 10
msg_len equ $ - msg

user_test_end:
