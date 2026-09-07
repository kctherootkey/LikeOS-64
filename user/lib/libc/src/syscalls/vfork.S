/*
 * vfork(2).
 *
 * Frameless on purpose.  The child runs on THIS stack, returns from this very
 * call into the caller, and its first call after that overwrites the slot
 * holding our return address -- so a wrapper with a frame of its own would
 * have nothing valid to return through once the parent resumes.  This stub
 * pushes nothing before the syscall; the kernel reads the eight bytes at the
 * stack pointer when the parent enters, and writes them back before the
 * parent returns (vfork_wait() in kernel/ke/fork.c).  The child sees them
 * intact because it returns before anything could have touched them.
 *
 * glibc keeps the return address in %rdi across the syscall instead.  That
 * relies on the child inheriting a caller-saved register, and this kernel's
 * fork_child_return zeroes them (they would otherwise leak kernel pointers),
 * so the slot is restored on the kernel side here.
 *
 * Error path only in the parent: the child gets 0 and returns at once.  The
 * pushes there go below the return slot and are the parent's own to use.
 */

	.text
	.global vfork
	.type vfork, @function
vfork:
	mov	$311, %eax		/* SYS_VFORK */
	syscall
	cmp	$-4095, %rax		/* -errno lands in [-4095, -1] */
	jae	1f
	ret
1:
	neg	%eax			/* errno, positive */
	push	%rax			/* keeps it, and aligns rsp for the call */
	call	__errno_location@PLT
	pop	%rcx			/* call-clobbered: free to use */
	mov	%ecx, (%rax)		/* errno is an int: 32-bit store */
	mov	$-1, %rax
	ret
	.size vfork, .-vfork

/* Non-executable stack */
	.section .note.GNU-stack,"",@progbits
