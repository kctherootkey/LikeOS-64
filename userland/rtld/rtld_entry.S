/* ============================================================================
 * rtld_entry.S — LikeOS-64 Dynamic Linker entry & PLT trampoline
 *
 *   _start               — initial entry from kernel
 *   _dl_runtime_resolve  — lazy PLT resolution trampoline
 * ========================================================================= */

    .text

/* ====================================================================
 * _start — Dynamic linker entry point
 *
 * Kernel stack layout on entry:
 *     [rsp+0]    argc
 *     [rsp+8..]  argv[], NULL, envp[], NULL, auxv[]
 *
 * Steps:
 *   1. Find our load base (scan backward for ELF magic)
 *   2. Self-relocate (apply R_X86_64_RELATIVE from our .rela.dyn)
 *   3. Call _dl_main(original_sp, own_base)  →  returns app entry
 *   4. Restore original stack, jump to application
 * ==================================================================== */

    .align 16
    .global _start
    .type   _start, @function
_start:
    movq    %rsp, %rbx              /* save original sp */
    xorq    %rbp, %rbp              /* clean frame pointer */

    /* ---- Find our load base (page-aligned scan for ELF magic) ---- */
    leaq    _start(%rip), %r12
    andq    $~0xFFF, %r12
.Lscan:
    cmpl    $0x464C457F, (%r12)     /* "\x7FELF" little-endian */
    je      .Lfound
    subq    $0x1000, %r12
    jmp     .Lscan
.Lfound:
    /* r12 = base address of ld-likeos.so */

    /* ---- Self-relocate: find PT_DYNAMIC ---- */
    movzwl  56(%r12), %ecx          /* e_phnum */
    movq    32(%r12), %rsi          /* e_phoff */
    addq    %r12, %rsi              /* rsi = first Elf64_Phdr */
.Lph_loop:
    testl   %ecx, %ecx
    jz      .Lno_dynamic
    cmpl    $2, (%rsi)              /* p_type == PT_DYNAMIC? */
    je      .Lgot_dynamic
    addq    $56, %rsi               /* next phdr */
    decl    %ecx
    jmp     .Lph_loop

.Lgot_dynamic:
    movq    16(%rsi), %rdi          /* p_vaddr of PT_DYNAMIC */
    addq    %r12, %rdi              /* runtime address of _DYNAMIC */

    /* Walk _DYNAMIC for DT_RELA(7) and DT_RELASZ(8) */
    xorq    %r13, %r13              /* rela ptr */
    xorq    %r14, %r14              /* rela size */
.Ldyn_loop:
    movq    (%rdi), %rax
    testq   %rax, %rax
    jz      .Ldyn_done
    cmpq    $7, %rax
    jne     1f
    movq    8(%rdi), %r13
    addq    %r12, %r13
    jmp     2f
1:  cmpq    $8, %rax
    jne     2f
    movq    8(%rdi), %r14
2:  addq    $16, %rdi
    jmp     .Ldyn_loop

.Ldyn_done:
    /* Apply R_X86_64_RELATIVE (type 8) relocations */
    testq   %r13, %r13
    jz      .Lno_dynamic
    testq   %r14, %r14
    jz      .Lno_dynamic

    xorq    %rcx, %rcx
.Lreloc_loop:
    cmpq    %r14, %rcx
    jge     .Lno_dynamic
    movq    8(%r13,%rcx), %rax      /* r_info */
    andl    $0xFFFFFFFF, %eax       /* R_TYPE = lower 32 bits */
    cmpl    $8, %eax                /* R_X86_64_RELATIVE? */
    jne     .Lreloc_next
    movq    (%r13,%rcx), %rdi       /* r_offset */
    movq    16(%r13,%rcx), %rax     /* r_addend */
    addq    %r12, %rax              /* base + addend */
    movq    %rax, (%r12,%rdi)       /* *(base + offset) = base + addend */
.Lreloc_next:
    addq    $24, %rcx
    jmp     .Lreloc_loop

.Lno_dynamic:
    /* ---- Call _dl_main(original_sp, own_base) ---- */
    movq    %rbx, %rdi              /* arg1: original sp */
    movq    %r12, %rsi              /* arg2: own_base */
    andq    $~0xF, %rsp             /* 16-byte align */
    call    _dl_main
    /* rax = application entry point */

    /* ---- Jump to app with original stack ---- */
    movq    %rbx, %rsp              /* restore original sp */
    xorq    %rbp, %rbp
    xorq    %rdx, %rdx              /* clear rdx (fini, unused) */
    jmp     *%rax

    .size _start, .-_start


/* ====================================================================
 * _dl_runtime_resolve — Lazy PLT resolution trampoline
 *
 * The standard x86-64 PLT convention:
 *   PLTn:  jmp  *GOT[n]          ; first time: falls through
 *          push $reloc_index     ; pushed SECOND
 *          jmp  PLT0
 *   PLT0:  push *GOT[1]          ; pushed FIRST (on top)
 *          jmp  *GOT[2]          ; → here
 *
 * Stack on entry to _dl_runtime_resolve:
 *   [rsp+ 0] = GOT[1] value   (dso_t *)   — pushed by PLT0
 *   [rsp+ 8] = reloc_index                — pushed by PLTn
 *   [rsp+16] = return address             — pushed by original 'call'
 *
 * We must:
 *   1. Save all argument/scratch registers
 *   2. Call _dl_fixup(dso_t *, reloc_index) → resolved addr in rax
 *   3. Restore registers
 *   4. Remove GOT[1] and reloc_index from stack
 *   5. Jump to resolved function (return addr still on stack)
 * ==================================================================== */

    .align 16
    .global _dl_runtime_resolve
    .type   _dl_runtime_resolve, @function
_dl_runtime_resolve:
    /*
     * Stack right now:
     *   [rsp+ 0] = GOT[1]        (dso_t *)
     *   [rsp+ 8] = reloc_index
     *   [rsp+16] = return address
     *
     * We'll use the GOT[1] slot to store the resolved address later.
     */

    /* Save argument registers (9 × 8 = 72 bytes) */
    pushq   %rax
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    /* Stack offsets from rsp:
     *   +0..+71 = saved regs
     *   +72     = GOT[1] (dso_t *)
     *   +80     = reloc_index
     *   +88     = return address
     */

    movq    72(%rsp), %rdi          /* arg1: dso_t *    */
    movq    80(%rsp), %rsi          /* arg2: reloc_index */
    call    _dl_fixup               /* rax = resolved address */

    /* Stash the resolved address where GOT[1] was.
     * After we restore regs and pop, we'll pop this into a register. */
    movq    %rax, 72(%rsp)

    /* Restore registers */
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rax

    /*
     * Stack now:
     *   [rsp+ 0] = resolved_addr  (was GOT[1])
     *   [rsp+ 8] = reloc_index
     *   [rsp+16] = return address
     *
     * Pop resolved_addr into r11, skip reloc_index, jump to r11.
     * Return address stays on stack for the target function.
     */
    popq    %r11                    /* resolved address */
    addq    $8, %rsp                /* skip reloc_index */
    jmp     *%r11                   /* enter resolved function; return addr on stack */

    .size _dl_runtime_resolve, .-_dl_runtime_resolve

/* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits
