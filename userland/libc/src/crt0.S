.section .text
.global _start
.global _entry

_entry:
_start:
    # Clear frame pointer
    xor %rbp, %rbp
    
    # The kernel sets up the stack with:
    #   [argc]        <- RSP points here
    #   [argv[0]] [argv[1]] ... [NULL]
    #   [envp[0]] [envp[1]] ... [NULL]
    
    # Load argc from stack
    mov (%rsp), %rdi       # argc at rsp
    
    # Calculate argv pointer (rsp + 8)
    lea 8(%rsp), %rsi      # argv starts at rsp+8
    
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
