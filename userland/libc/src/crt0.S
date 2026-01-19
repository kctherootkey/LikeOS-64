.section .text
.global _start
.global _entry

_entry:
_start:
    # Clear frame pointer
    xor %rbp, %rbp
    
    # The kernel sets up the stack with:
    #   [argc]
    #   [argv[0]] [argv[1]] ... [NULL]
    #   [envp[0]] [envp[1]] ... [NULL]
    # RSP points 8 bytes before argc when we start
    
    # Load argc from stack BEFORE alignment
    mov 8(%rsp), %rdi      # argc at rsp+8
    
    # Calculate argv pointer (rsp + 16)
    lea 16(%rsp), %rsi     # argv starts at rsp+16
    
    # Calculate envp pointer
    # envp = argv + (argc + 1) * 8
    mov %rdi, %rax         # rax = argc
    inc %rax               # rax = argc + 1
    shl $3, %rax           # rax = (argc + 1) * 8
    lea (%rsi, %rax), %rdx # rdx = argv + offset = envp
    
    # Now align stack to 16 bytes (arguments are already in registers)
    and $-16, %rsp
    
    # Call main(argc, argv, envp)
    call main
    
    # Exit with return value from main
    mov %rax, %rdi
    mov $60, %rax      # SYS_exit
    syscall
    
    # Should never get here
    hlt
