/*
 * ucontext.S -- getcontext / setcontext / swapcontext for x86-64.
 *
 * Register slots are uc_mcontext.gregs[REG_*] at byte offsets computed from
 * the ucontext_t layout in <sys/ucontext.h>:
 *
 *   uc_flags        0
 *   uc_link         8
 *   uc_stack       16   (ss_sp 16, ss_flags 24, ss_size 32)
 *   uc_mcontext    40   gregs[i] at 40 + 8*i
 *                       fpregs at 40 + 8*23 = 224
 *   uc_sigmask     296
 *   __reserved     304
 *   __fpregs_mem   368  (16-byte aligned by the attribute; 368 already is)
 *
 * Only the callee-saved registers, the stack pointer, the resume address,
 * the argument registers (so a context can be resumed into a function that
 * has not yet consumed them) and the FPU environment are recorded; the
 * caller-saved rest is dead at a function-call boundary, which is what
 * these are.
 */
#define oRBP    120
#define oRSP    160
#define oRBX    128
#define oR8      40
#define oR9      48
#define oR12     72
#define oR13     80
#define oR14     88
#define oR15     96
#define oRDI    104
#define oRSI    112
#define oRDX    136
#define oRAX    144
#define oRCX    152
#define oRIP    168
#define oFPREGS 224
#define oSIGMASK 296
#define oFPREGSMEM 368

#define SIG_BLOCK 0
#define SIG_SETMASK 2

	.section .text

/* int getcontext(ucontext_t *ucp) */
	.globl	getcontext
	.type	getcontext, @function
getcontext:
	movq	%rbx, oRBX(%rdi)
	movq	%rbp, oRBP(%rdi)
	movq	%r12, oR12(%rdi)
	movq	%r13, oR13(%rdi)
	movq	%r14, oR14(%rdi)
	movq	%r15, oR15(%rdi)
	movq	%rdi, oRDI(%rdi)
	movq	%rsi, oRSI(%rdi)
	movq	%rdx, oRDX(%rdi)
	movq	%rcx, oRCX(%rdi)
	movq	%r8, oR8(%rdi)
	movq	%r9, oR9(%rdi)
	movq	(%rsp), %rcx		/* return address */
	movq	%rcx, oRIP(%rdi)
	leaq	8(%rsp), %rcx		/* stack pointer after our return */
	movq	%rcx, oRSP(%rdi)
	/* FPU environment: control words, x87 stack, MXCSR, XMM. */
	leaq	oFPREGSMEM(%rdi), %rcx
	movq	%rcx, oFPREGS(%rdi)
	fxsave	(%rcx)
	/* Signal mask: sigprocmask(SIG_BLOCK, NULL, &ucp->uc_sigmask). */
	leaq	oSIGMASK(%rdi), %rdx
	xorl	%esi, %esi
	movl	$SIG_BLOCK, %edi
	subq	$8, %rsp		/* keep 16-byte alignment for the call */
	call	sigprocmask@PLT
	addq	$8, %rsp
	xorl	%eax, %eax
	ret
	.size	getcontext, .-getcontext

/* int setcontext(const ucontext_t *ucp) */
	.globl	setcontext
	.type	setcontext, @function
setcontext:
	pushq	%rdi
	/* sigprocmask(SIG_SETMASK, &ucp->uc_sigmask, NULL) */
	leaq	oSIGMASK(%rdi), %rsi
	xorl	%edx, %edx
	movl	$SIG_SETMASK, %edi
	call	sigprocmask@PLT
	popq	%rdi
	testl	%eax, %eax
	jnz	1f
	movq	oFPREGS(%rdi), %rcx
	fxrstor	(%rcx)
	movq	oRSP(%rdi), %rsp
	movq	oRBX(%rdi), %rbx
	movq	oRBP(%rdi), %rbp
	movq	oR12(%rdi), %r12
	movq	oR13(%rdi), %r13
	movq	oR14(%rdi), %r14
	movq	oR15(%rdi), %r15
	movq	oRIP(%rdi), %rcx
	pushq	%rcx			/* resume address, popped by ret */
	movq	oRSI(%rdi), %rsi
	movq	oRDX(%rdi), %rdx
	movq	oRCX(%rdi), %rcx
	movq	oR8(%rdi), %r8
	movq	oR9(%rdi), %r9
	movq	oRDI(%rdi), %rdi
	xorl	%eax, %eax
	ret
1:	movl	$-1, %eax
	ret
	.size	setcontext, .-setcontext

/* int swapcontext(ucontext_t *oucp, const ucontext_t *ucp) */
	.globl	swapcontext
	.type	swapcontext, @function
swapcontext:
	/* Save into oucp exactly as getcontext does ... */
	movq	%rbx, oRBX(%rdi)
	movq	%rbp, oRBP(%rdi)
	movq	%r12, oR12(%rdi)
	movq	%r13, oR13(%rdi)
	movq	%r14, oR14(%rdi)
	movq	%r15, oR15(%rdi)
	movq	%rdi, oRDI(%rdi)
	movq	%rsi, oRSI(%rdi)
	movq	%rdx, oRDX(%rdi)
	movq	%rcx, oRCX(%rdi)
	movq	%r8, oR8(%rdi)
	movq	%r9, oR9(%rdi)
	movq	(%rsp), %rcx
	movq	%rcx, oRIP(%rdi)
	leaq	8(%rsp), %rcx
	movq	%rcx, oRSP(%rdi)
	leaq	oFPREGSMEM(%rdi), %rcx
	movq	%rcx, oFPREGS(%rdi)
	fxsave	(%rcx)
	/* ... swap masks in one call: sigprocmask(SIG_SETMASK, &ucp->mask,
	 * &oucp->mask) ... */
	pushq	%rsi			/* ucp, and keeps alignment */
	leaq	oSIGMASK(%rdi), %rdx
	leaq	oSIGMASK(%rsi), %rsi
	movl	$SIG_SETMASK, %edi
	call	sigprocmask@PLT
	popq	%rdi			/* ucp */
	testl	%eax, %eax
	jnz	1f
	/* ... and resume ucp as setcontext does. */
	movq	oFPREGS(%rdi), %rcx
	fxrstor	(%rcx)
	movq	oRSP(%rdi), %rsp
	movq	oRBX(%rdi), %rbx
	movq	oRBP(%rdi), %rbp
	movq	oR12(%rdi), %r12
	movq	oR13(%rdi), %r13
	movq	oR14(%rdi), %r14
	movq	oR15(%rdi), %r15
	movq	oRIP(%rdi), %rcx
	pushq	%rcx
	movq	oRSI(%rdi), %rsi
	movq	oRDX(%rdi), %rdx
	movq	oRCX(%rdi), %rcx
	movq	oR8(%rdi), %r8
	movq	oR9(%rdi), %r9
	movq	oRDI(%rdi), %rdi
	xorl	%eax, %eax
	ret
1:	movl	$-1, %eax
	ret
	.size	swapcontext, .-swapcontext

/* Entered when a makecontext()'d function returns: %rbx holds uc_link. */
	.globl	__start_context
	.type	__start_context, @function
__start_context:
	movq	%rbx, %rdi
	testq	%rdi, %rdi
	je	1f
	call	setcontext@PLT
1:	xorl	%edi, %edi
	call	exit@PLT
	hlt
	.size	__start_context, .-__start_context

	.section .note.GNU-stack,"",@progbits
