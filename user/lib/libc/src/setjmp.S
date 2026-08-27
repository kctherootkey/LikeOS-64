/*
 * setjmp.S — x86-64 setjmp/longjmp/sigsetjmp/siglongjmp for LikeOS
 *
 * jmp_buf layout (unsigned long[8]):
 *   [0] = %rbx        (8 bytes each, 0-based slot index × 8 = byte offset)
 *   [1] = %rbp
 *   [2] = %r12
 *   [3] = %r13
 *   [4] = %r14
 *   [5] = %r15
 *   [6] = %rsp  (pre-call value: caller's %rsp + 8)
 *   [7] = %rip  (return address from caller's stack)
 *
 * sigjmp_buf layout (unsigned long[9]):
 *   [0..7] = same as jmp_buf
 *   [8]    = signal mask placeholder (not used; signal mask save/restore
 *            is not supported — savemask is accepted but ignored)
 */

    .section .text

/* ------------------------------------------------------------------ */
/* setjmp(jmp_buf env)                                                  */
/* Save callee-saved registers and return address.  Returns 0.          */
/* ------------------------------------------------------------------ */
    .globl  setjmp
    .type   setjmp, @function
setjmp:
    movq    %rbx,  0(%rdi)
    movq    %rbp,  8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rax       /* pre-call %rsp (before this call's ret-addr) */
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax        /* saved %rip = return address on stack */
    movq    %rax, 56(%rdi)
    xorl    %eax, %eax
    ret
    .size   setjmp, .-setjmp

/* ------------------------------------------------------------------ */
/* longjmp(jmp_buf env, int val)                                        */
/* Restore registers and jump back.  Never returns.                     */
/* If val == 0, returns 1 to the setjmp call site (POSIX requirement).  */
/* ------------------------------------------------------------------ */
    .globl  longjmp
    .type   longjmp, @function
longjmp:
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax
1:
    movq     0(%rdi), %rbx
    movq     8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    jmpq    *56(%rdi)
    .size   longjmp, .-longjmp

/* ------------------------------------------------------------------ */
/* _setjmp / _longjmp                                                   */
/*                                                                      */
/* These are the variants POSIX defines as explicitly NOT saving or     */
/* restoring the signal mask.  setjmp() above already does not touch    */
/* it -- see the sigjmp_buf note at the top of this file -- so on this  */
/* platform the two pairs have identical semantics, and these are       */
/* aliases rather than copies.  That is not a shortcut: _setjmp's       */
/* contract is "no signal mask", and this is the implementation that    */
/* honours it.  If setjmp ever learns to save the mask, these must      */
/* become separate implementations that still do not.                   */
/*                                                                      */
/* They are missing often enough to matter: Lua 5.1 selects them for    */
/* any POSIX build (LUA_USE_POSIX implies LUA_USE_ULONGJMP), and        */
/* without them liblua.a links nowhere -- "undefined reference to       */
/* `_longjmp'" out of every luaD_throw.                                 */
/* ------------------------------------------------------------------ */
    .globl  _setjmp
    .type   _setjmp, @function
    .set    _setjmp, setjmp

    .globl  _longjmp
    .type   _longjmp, @function
    .set    _longjmp, longjmp

/* ------------------------------------------------------------------ */
/* sigsetjmp(sigjmp_buf env, int savemask)                              */
/* savemask is accepted but signal mask is NOT saved (not supported).   */
/* ------------------------------------------------------------------ */
    .globl  sigsetjmp
    .type   sigsetjmp, @function
sigsetjmp:
    movq    %rbx,  0(%rdi)
    movq    %rbp,  8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rax
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)
    movq    $0, 64(%rdi)        /* signal mask slot — unused */
    xorl    %eax, %eax
    ret
    .size   sigsetjmp, .-sigsetjmp

/* ------------------------------------------------------------------ */
/* siglongjmp(sigjmp_buf env, int val)                                  */
/* Signal mask is NOT restored.  Delegates to longjmp logic.            */
/* ------------------------------------------------------------------ */
    .globl  siglongjmp
    .type   siglongjmp, @function
siglongjmp:
    movl    %esi, %eax
    testl   %eax, %eax
    jnz     2f
    movl    $1, %eax
2:
    movq     0(%rdi), %rbx
    movq     8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    jmpq    *56(%rdi)
    .size   siglongjmp, .-siglongjmp

    .section .note.GNU-stack,"",@progbits
