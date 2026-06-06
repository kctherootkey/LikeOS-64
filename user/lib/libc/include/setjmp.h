#ifndef _SETJMP_H
#define _SETJMP_H

#include <sys/cdefs.h>

__BEGIN_DECLS

/* x86-64 jmp_buf layout (indices × 8 = byte offset):
 *   [0] rbx  [1] rbp  [2] r12  [3] r13
 *   [4] r14  [5] r15  [6] rsp  [7] rip */
typedef unsigned long jmp_buf[8];

/* sigjmp_buf: same as jmp_buf plus one reserved slot.
 * Signal mask save/restore is not implemented; savemask is ignored. */
typedef unsigned long sigjmp_buf[9];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((__noreturn__));

int  sigsetjmp(sigjmp_buf env, int savemask);
void siglongjmp(sigjmp_buf env, int val) __attribute__((__noreturn__));

__END_DECLS

#endif /* _SETJMP_H */
