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
    ; Entry point for user task
    ; Simple test: write a message and exit

    ; First, get our PID
    mov rax, SYS_GETPID
    syscall
    ; RAX now has PID, save it
    mov r12, rax
    
    ; Write "Hello from user mode!" to stdout
    mov rax, SYS_WRITE      ; syscall number
    mov rdi, 1              ; fd = stdout
    lea rsi, [rel msg]      ; buffer
    mov rdx, msg_len        ; length
    syscall

    ; Loop a few times, yielding each time
    mov r13, 3              ; loop counter
.loop:
    ; Yield to show cooperative scheduling works
    mov rax, SYS_YIELD
    syscall
    
    dec r13
    jnz .loop

    ; Exit with success
    mov rax, SYS_EXIT
    mov rdi, 0              ; exit code 0
    syscall

    ; Should not reach here
.hang:
    jmp .hang

; Message to print
msg:
    db "[USER] Hello from user mode! Task running in Ring 3", 10, 0
msg_len equ $ - msg

user_test_end:
