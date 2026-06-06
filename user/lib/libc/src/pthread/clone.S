/*
 * LikeOS-64 clone() system call wrapper
 *
 * The clone() syscall requires special handling because the child thread
 * must start executing at a specified entry point with its own stack,
 * not return from the syscall like the parent does.
 *
 * Clone syscall signature:
 *   long sys_clone(flags, child_stack, parent_tidptr, child_tidptr, tls)
 *   Arguments: rdi=flags, rsi=stack, rdx=parent_tid, r10=child_tid, r8=tls
 *
 * Userspace clone() signature:
 *   pid_t clone(int (*fn)(void*), void* stack, int flags, void* arg, 
 *               pid_t* parent_tid, void* tls, pid_t* child_tid)
 *
 * We need to:
 *   1. Save fn and arg on child's stack (since child gets new stack)
 *   2. Call sys_clone
 *   3. Parent: return child's TID
 *   4. Child: pop fn/arg from stack, call fn(arg), then exit with result
 */

.global clone
.type clone, @function

clone:
    // Arguments:
    //   rdi = fn        (function pointer)
    //   rsi = stack     (child's stack top)
    //   rdx = flags     (clone flags)
    //   rcx = arg       (argument for fn)
    //   r8  = parent_tid (pointer to store parent's TID)
    //   r9  = tls       (thread-local storage pointer)
    //   [rsp+8] = child_tid (pointer to store child's TID)

    // Validate fn and stack are non-null
    test    %rdi, %rdi
    jz      .Lerror_inval
    test    %rsi, %rsi
    jz      .Lerror_inval

    // Save callee-saved registers
    push    %rbp
    push    %rbx
    push    %r12
    push    %r13
    push    %r14
    push    %r15

    // Save fn and arg in callee-saved registers
    mov     %rdi, %r12          // r12 = fn
    mov     %rcx, %r13          // r13 = arg

    // Align child's stack to 16 bytes and reserve space for fn/arg
    // We push fn and arg onto child's stack so child can access them
    and     $-16, %rsi          // Align stack
    sub     $16, %rsi           // Reserve space for 2 quadwords
    mov     %rdi, 0(%rsi)       // Store fn at stack[0]
    mov     %rcx, 8(%rsi)       // Store arg at stack[8]

    // Prepare syscall arguments:
    //   rax = SYS_CLONE (310)
    //   rdi = flags (was rdx)
    //   rsi = child_stack (already in rsi, adjusted)
    //   rdx = parent_tidptr (was r8)
    //   r10 = child_tidptr (from stack argument)
    //   r8  = tls (was r9)
    
    mov     %rdx, %rdi          // rdi = flags
    // rsi = stack (already set, modified above)
    mov     %r8, %rdx           // rdx = parent_tid
    mov     56(%rsp), %r10      // r10 = child_tid (7 pushes * 8 = 56 offset)
    mov     %r9, %r8            // r8 = tls

    // Make the syscall
    mov     $310, %rax          // SYS_CLONE = 310
    syscall

    // Check if we're the parent or child
    test    %rax, %rax
    jz      .Lchild             // rax == 0 means child
    jl      .Lparent_error      // rax < 0 means error

    // Parent: return child TID
.Lparent_success:
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbx
    pop     %rbp
    ret

.Lparent_error:
    // Error: rax contains negative errno
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbx
    pop     %rbp
    // Set errno and return -1
    neg     %rax                // Make errno positive
#ifdef __PIC__
    mov     errno@GOTPCREL(%rip), %rcx
    mov     %rax, (%rcx)
#else
    mov     %rax, errno(%rip)   // Store in errno
#endif
    mov     $-1, %rax
    ret

.Lchild:
    // Child thread starts here with new stack
    // Stack contains: [fn, arg] (pushed by parent above)
    xor     %rbp, %rbp          // Clear frame pointer (new thread)

    // Pop fn and arg from stack
    pop     %rax                // rax = fn
    pop     %rdi                // rdi = arg

    // Call fn(arg)
    call    *%rax

    // fn returned, exit with its return value
    mov     %rax, %rdi          // rdi = return value for exit
    mov     $60, %rax           // SYS_EXIT = 60
    syscall

    // Should never reach here
    ud2

.Lerror_inval:
    mov     $22, %rax           // EINVAL = 22
#ifdef __PIC__
    mov     errno@GOTPCREL(%rip), %rcx
    mov     %rax, (%rcx)
#else
    mov     %rax, errno(%rip)
#endif
    mov     $-1, %rax
    ret

.size clone, .-clone

/* Non-executable stack */
.section .note.GNU-stack,"",@progbits
