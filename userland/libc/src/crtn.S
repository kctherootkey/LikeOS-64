/* crtn.S — _init / _fini function epilogues for ELF shared libraries (x86-64)
 *
 * These fragments close the _init and _fini functions whose prologues are in
 * crti.S.  The linker concatenates .init sections in link order, so the final
 * _init function is:
 *
 *   crti.o prologue (push %rbp / mov %rsp,%rbp)
 *     ... body fragments from intermediate objects (e.g. call cpuid_setup) ...
 *   crtn.o epilogue (pop %rbp / ret)
 */

        .section .init,"ax",@progbits
        popq    %rbp
        ret

        .section .fini,"ax",@progbits
        popq    %rbp
        ret

        .section .note.GNU-stack,"",@progbits
