/* crt1.S — C runtime startup for dynamically linked programs
 *
 * When the dynamic linker transfers control here, the stack contains:
 *     [rsp+0]   argc
 *     [rsp+8..]  argv[], NULL, envp[], NULL, auxv[]
 *
 * This is the same layout as crt0.S, but for PIE executables that
 * are loaded by ld-likeos.so.
 */

    .section .text
    .global _start
    .global _entry
    .global __restore_rt

_entry:
_start:
    /* Clear frame pointer */
    xor     %rbp, %rbp

    /* argc */
    mov     (%rsp), %rdi

    /* argv = rsp + 8 */
    lea     8(%rsp), %rsi

    /* envp = argv + (argc + 1) * 8 */
    mov     %rdi, %rax
    inc     %rax
    shl     $3, %rax
    lea     (%rsi, %rax), %rdx

    /* Align stack to 16 bytes */
    and     $-16, %rsp

    /* Save argc, argv, envp in callee-saved registers across call */
    mov     %rdi, %rbx         /* rbx = argc  */
    mov     %rsi, %r12         /* r12 = argv  */
    mov     %rdx, %r13         /* r13 = envp  */

    /* Populate libc environment storage from envp before main */
    mov     %rdx, %rdi
    call    __libc_init_environ

    /* Set __progname from argv[0] */
    mov     (%r12), %rdi        /* argv[0] */
    call    __libc_set_progname

    /* Run the executable's constructors (.preinit_array/.init_array).
     *
     * This has to happen HERE.  The dynamic linker runs constructors for
     * every shared library but deliberately skips the main object, because
     * this is the code that is supposed to do that one.  Without it, no
     * __attribute__((constructor)) function and no C++ static initialiser in
     * a program ever runs -- silently, with no diagnostic. */
    mov     %rbx, %rdi
    mov     %r12, %rsi
    mov     %r13, %rdx
    call    __libc_run_init_array

    /* Restore argc, argv, envp for main(argc, argv, envp) */
    mov     %rbx, %rdi
    mov     %r12, %rsi
    mov     %r13, %rdx

    /* Call main(argc, argv, envp) */
    call    main

    /* Return from main is equivalent to exit(main(...)): exit() flushes
     * stdio, runs atexit handlers and the destructors.  Going straight to
     * the exit syscall here would skip all three. */
    mov     %rax, %rdi
    call    exit
    hlt

/* Signal return trampoline */
__restore_rt:
    mov     $256, %rax
    syscall
    hlt

    .section .note.GNU-stack,"",@progbits
