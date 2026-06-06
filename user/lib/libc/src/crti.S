/* crti.S — _init / _fini function prologues for ELF shared libraries (x86-64)
 *
 * The GNU linker merges .init sections from all linked objects in link order:
 *   crti.o (_init prologue)  →  [each object's .init body]  →  crtn.o (epilogue)
 *
 * The resulting _init function is registered as DT_INIT and called by the
 * dynamic linker immediately after the library is loaded and relocated.
 * _fini is registered as DT_FINI and called on library unload.
 *
 * Stack alignment: we push %rbp, which drops RSP by 8.  The call instruction
 * that invokes _init itself pushed a return address (8 bytes), so after the
 * push RSP is 16-byte aligned — matching the ABI requirement for any call
 * inside the .init body.
 */

        .section .init,"ax",@progbits
        .globl  _init
        .type   _init,@function
_init:
        push    %rbp
        movq    %rsp,%rbp

        .section .fini,"ax",@progbits
        .globl  _fini
        .type   _fini,@function
_fini:
        push    %rbp
        movq    %rsp,%rbp

        .section .note.GNU-stack,"",@progbits
