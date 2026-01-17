; LikeOS-64 User Mode Startup
; This must be linked first to be at the start of the binary

BITS 64
SECTION .text.start

global _entry
extern _start
extern __bss_start
extern _end

_entry:
    ; Clear frame pointer for backtraces
    xor rbp, rbp
    
    ; Zero BSS section
    lea rdi, [rel __bss_start]
    lea rcx, [rel _end]
    sub rcx, rdi               ; rcx = size of BSS
    shr rcx, 3                 ; convert to qwords
    xor rax, rax
    rep stosq
    
    ; Call the C entry point
    call _start
    
    ; If _start returns, exit with return value
    mov rdi, rax
    mov rax, 60         ; SYS_EXIT
    syscall
    
    ; Should never reach here
    jmp $
