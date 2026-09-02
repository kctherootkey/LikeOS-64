/* <sys/ucontext.h> -- the machine context a signal handler is handed.
 *
 * The third argument of an SA_SIGINFO handler points at a ucontext_t: the
 * registers the interrupted code was running with, its signal mask and its
 * alternate-stack state.  The layout is the one the kernel writes into the
 * signal frame (include/kernel/ke/signal.h) and MUST match it field for
 * field; the kernel reads it back at sigreturn, so a handler that edits
 * uc_mcontext.gregs[REG_RIP] changes where the interrupted code resumes.
 * That is the documented use of it -- a runtime recovering from a fault it
 * arranged on purpose, a garbage collector reading a suspended thread's
 * registers -- and it works here.
 *
 * The register order in gregs[] is the conventional x86-64 one, so code
 * written against the REG_* names of other systems compiles unchanged. */
#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H

#include <sys/types.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long long int greg_t;

enum {
	REG_R8 = 0,
#define REG_R8 REG_R8
	REG_R9,
#define REG_R9 REG_R9
	REG_R10,
#define REG_R10 REG_R10
	REG_R11,
#define REG_R11 REG_R11
	REG_R12,
#define REG_R12 REG_R12
	REG_R13,
#define REG_R13 REG_R13
	REG_R14,
#define REG_R14 REG_R14
	REG_R15,
#define REG_R15 REG_R15
	REG_RDI,
#define REG_RDI REG_RDI
	REG_RSI,
#define REG_RSI REG_RSI
	REG_RBP,
#define REG_RBP REG_RBP
	REG_RBX,
#define REG_RBX REG_RBX
	REG_RDX,
#define REG_RDX REG_RDX
	REG_RAX,
#define REG_RAX REG_RAX
	REG_RCX,
#define REG_RCX REG_RCX
	REG_RSP,
#define REG_RSP REG_RSP
	REG_RIP,
#define REG_RIP REG_RIP
	REG_EFL,
#define REG_EFL REG_EFL
	REG_CSGSFS, /* code segment in the low 16 bits */
#define REG_CSGSFS REG_CSGSFS
	REG_ERR,
#define REG_ERR REG_ERR
	REG_TRAPNO,
#define REG_TRAPNO REG_TRAPNO
	REG_OLDMASK,
#define REG_OLDMASK REG_OLDMASK
	REG_CR2, /* faulting address for SIGSEGV/SIGBUS */
#define REG_CR2 REG_CR2
	__NGREG
};
#define NGREG __NGREG

typedef greg_t gregset_t[NGREG];

/* The extended register image: FXSAVE layout for the first 512 bytes
 * (control words, x87 stack, XMM registers), followed on a CPU with XSAVE
 * by the XSAVE header and the AVX/AVX-512 components. */
struct _libc_fpstate {
	unsigned short cwd;
	unsigned short swd;
	unsigned short ftw;
	unsigned short fop;
	unsigned long long rip;
	unsigned long long rdp;
	unsigned int mxcsr;
	unsigned int mxcr_mask;
	struct {
		unsigned short significand[4];
		unsigned short exponent;
		unsigned short padding[3];
	} _st[8];
	struct {
		unsigned int element[4];
	} _xmm[16];
	unsigned int padding[24];
};
typedef struct _libc_fpstate *fpregset_t;

typedef struct {
	gregset_t gregs;
	fpregset_t fpregs;
	unsigned long long __reserved1[8];
} mcontext_t;

typedef struct ucontext_t {
	unsigned long int uc_flags;
	struct ucontext_t *uc_link;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
	sigset_t uc_sigmask;
	unsigned char __reserved[64];
	/* Used by getcontext()/makecontext() for the FPU environment; the
	 * kernel's signal frames keep their image elsewhere and point
	 * uc_mcontext.fpregs at it. */
	struct _libc_fpstate __fpregs_mem __attribute__((aligned(16)));
} ucontext_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UCONTEXT_H */
