// LikeOS-64 -- execve.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/elf.h>
#include <kernel/ke/script_loader.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/namei.h>


// SYS_EXECVE - execute a new program, replacing current process image
// This is the POSIX-compliant version that replaces the current task
/* Per-exec-level DAC screen: stat the target (also needed later for set-id
 * application) and, for a non-root caller, require search on every ancestor
 * directory plus execute on the file itself.  Permissive if it can't be
 * stat'd (e.g. a relative path elf_exec_replace resolves itself).  Run once
 * for the exec target and once per shebang interpreter level. */
static int execve_check_exec(const char *kpath, struct kstat *xst,
			     int *have_xst)
{
	*have_xst = (vfs_stat(kpath, xst) == ST_OK);
	task_t *cur = sched_current();
	if (cur && cur->cred.euid != 0) {
		int pr = perm_traverse(kpath); /* search on ancestor dirs */
		if (pr == 0 && *have_xst) /* + execute on the file itself */
			pr = perm_access(cur, kpath, xst, MAY_EXEC, 0);
		if (pr < 0)
			return pr;
	}
	return 0;
}


int64_t sys_execve(uint64_t pathname, uint64_t argv_ptr,
			  uint64_t envp_ptr)
{
	if (!validate_user_ptr(pathname, 1)) {
		return -EFAULT;
	}

	const char *user_path = (const char *)pathname;
	const char *const *user_argv = (const char *const *)argv_ptr;
	const char *const *user_envp = (const char *const *)envp_ptr;

	char *kpath = NULL;
	char **kargv = NULL;
	char **kenvp = NULL;

	int ret = copy_user_string(user_path, VFS_MAX_PATH, &kpath, NULL);
	if (ret != 0) {
		return ret;
	}

	ret = copy_user_string_array(user_argv, 128, 4096, 16384, &kargv);
	if (ret != 0) {
		kfree(kpath);
		return ret;
	}

	ret = copy_user_string_array(user_envp, 128, 4096, 16384, &kenvp);
	if (ret != 0) {
		free_user_string_array(kargv);
		kfree(kpath);
		return ret;
	}

	/* Exec-permission screen + shebang (#!) resolution.  Each iteration
	 * checks DAC on the current target (denying BEFORE the image is
	 * replaced, after which an error can no longer be returned) and then
	 * asks the script loader to rewrite path/argv one interpreter level.
	 * The loop ends at the first non-script target; xst then describes
	 * the FINAL binary, so set-id bits on scripts are naturally ignored
	 * while an interpreter's own set-id bits still apply.  kenvp is never
	 * touched: the environment passes through unchanged. */
	struct kstat xst;
	int have_xst = 0;
	for (int depth = 0;; depth++) {
		ret = execve_check_exec(kpath, &xst, &have_xst);
		if (ret < 0)
			goto out_err;
		int sr = script_load_rewrite(&kpath, &kargv, depth);
		if (sr == 0)
			break; /* not a script: kpath is the final binary */
		if (sr < 0) {
			ret = sr;
			goto out_err;
		}
		/* sr == 1: kpath now names the interpreter; re-check it */
	}
	ret = script_check_stack_fit(kargv, kenvp);
	if (ret < 0)
		goto out_err;

	/* Work out the identity the new image will run with BEFORE the image is
	 * replaced, because the auxv built during the replace has to report it
	 * (AT_SECURE, AT_EUID -- see elf_setup_stack).  Reading the task from
	 * in there instead described the CALLER, which is how AT_SECURE came to
	 * be reported as 0 for every setuid exec: the transition below had not
	 * happened yet.
	 *
	 * Nothing is committed here.  A failed exec has to leave the caller
	 * exactly as it was, still running its old program, so the ids are only
	 * applied once the replace has actually succeeded. */
	cred_t newcred;
	int have_newcred = 0;
	int new_dumpable = 1;
	{
		task_t *cur = sched_current();
		if (cur) {
			uint32_t old_euid = cur->cred.euid;
			uint32_t old_egid = cur->cred.egid;

			newcred = cur->cred;
			have_newcred = 1;

			/* Set-user-ID / set-group-ID on exec (04000 / 02000).
			 * The real IDs are unchanged; effective+saved+fs IDs
			 * take the file's.  xst describes the FINAL binary, so
			 * set-id bits on a #! script are ignored while the
			 * interpreter's own still apply. */
			if (have_xst) {
				if (xst.st_mode & 04000) { /* S_ISUID */
					newcred.euid = newcred.suid =
						newcred.fsuid =
							(uint32_t)xst.st_uid;
				}
				if (xst.st_mode & 02000) { /* S_ISGID */
					newcred.egid = newcred.sgid =
						newcred.fsgid =
							(uint32_t)xst.st_gid;
				}
			}

			/* A traced process does not gain privilege here.
			 *
			 * Its tracer already holds complete control of its
			 * memory and registers, so letting the image raise the
			 * ids would hand the tracer's owner everything the new
			 * identity can reach -- attaching to a program that is
			 * about to exec a set-id binary would otherwise be a
			 * way to inherit that binary's privileges.
			 *
			 * The conventional rule degrades the exec rather than
			 * refusing it: the program still runs, just as itself.
			 * Refusing would break debugging anything that execs a
			 * set-id helper, for no extra safety.  A privileged
			 * tracer changes nothing -- it could assume the
			 * identity anyway, so there is nothing to protect.
			 *
			 * Applied before the dumpable decision below so that
			 * decision sees the ids the image will really run
			 * with. */
			if ((newcred.euid != old_euid ||
			     newcred.egid != old_egid) &&
			    task_traced_by_unprivileged(cur))
				newcred = cur->cred;

			/* An image that gained ids here can read what its new
			 * identity reaches and its owner's could not, so it
			 * stops being theirs to read or to trace.  Recorded at
			 * exec and deliberately NOT recomputed afterwards: a
			 * setuid-root program that drops all the way back with
			 * setuid() ends up with ids identical to its owner's
			 * while still holding whatever it read as root, and the
			 * id comparison alone would hand that over.
			 *
			 * A process that was already non-dumpable stays that
			 * way: exec'ing something ordinary does not unlearn
			 * what the previous image was trusted with. */
			if (newcred.euid != old_euid || newcred.egid != old_egid ||
			    newcred.uid != newcred.euid ||
			    newcred.gid != newcred.egid ||
			    !task_dumpable(cur))
				new_dumpable = 0;
		}
	}

	uint64_t new_stack_ptr = 0;
	uint64_t entry_point =
		elf_exec_replace(kpath, kargv, kenvp, &new_stack_ptr,
				 have_newcred ? &newcred : NULL);

	if (entry_point == 0) {
		// exec failed, return error to caller
		ret = -ENOEXEC;
		goto out_err;
	}

	/* POSIX: a successful exec resets caught signal handlers to their
	 * default disposition (ignored signals stay ignored).  The old handler
	 * addresses belong to the previous program image and would crash if a
	 * signal were delivered to them in the new one. */
	{
		task_t *cur = sched_current();
		if (cur)
			signal_reset_on_exec(cur);
	}

	/* Commit the identity worked out before the replace.  Only now: the old
	 * image is gone and no error can still be returned, so this is the
	 * first point at which raising the ids cannot leave a process
	 * privileged in a program it was not supposed to be running. */
	{
		task_t *cur = sched_current();
		if (cur && have_newcred) {
			cur->cred = newcred;
			task_set_dumpable(cur, new_dumpable);
		}
	}

	/* Flags the new image starts with.
	 *
	 * Built here rather than taken from the current kernel flags with
	 * pushfq: a fresh program should not inherit whatever the kernel
	 * happened to be carrying, and one bit has to survive deliberately.
	 * If a tracer single-stepped into this exec, the trap flag it set is
	 * in the saved user flags, and this path does NOT return through the
	 * syscall frame that normally carries it -- it builds its own IRET
	 * frame below.  Dropping it here would silently turn a step into a
	 * run, with the tracee never stopping again. */
	uint64_t new_rflags = 0x202; /* reserved bit 1, IF */
	{
		task_t *c = sched_current();

		if (c && (c->syscall_rflags & 0x100ULL))
			new_rflags |= 0x100ULL; /* TF */
	}

	/* Stop for the tracer at the boundary between the two images.
	 *
	 * This is what makes the fork + PTRACE_TRACEME + execve sequence work:
	 * the child stops the moment the new program is loaded and before a
	 * single one of its instructions runs, which is the only point at which
	 * a debugger can plant breakpoints in it before it starts.  Without it
	 * the child would run to completion the instant it was resumed and
	 * there would be nothing to debug.
	 *
	 * Delivered as SIGTRAP; the tracer sees a stop with PTRACE_EVENT_EXEC
	 * and resumes with PTRACE_CONT once it is ready. */
	{
		task_t *cur = sched_current();

		if (cur) {
			/* The saved user context now describes the NEW image.
			 *
			 * Done for every exec, not just a traced one, because it
			 * is simply true: the old image is gone, and anything
			 * that reads this task's user registers -- signal
			 * delivery builds its frame from them -- would otherwise
			 * be describing a program that no longer exists.
			 *
			 * The syscall frame is dropped for the same reason, and
			 * it matters more than it looks: execve does not return
			 * through it.  It builds an IRET frame below and jumps.
			 * Left in place it still held the address of the execve
			 * call site in the OLD image, and because it takes
			 * priority over these fields when registers are read, a
			 * tracer at this stop was told the program starts at an
			 * address that had just been unmapped -- which is
			 * exactly as useful as it sounds. */
			cur->syscall_frame = NULL;
			/* execve has no exit stop to pair with its entry one --
			 * it never returns -- so the in-progress marker would
			 * stay set and swallow the NEXT syscall's entry stop,
			 * after which every reported boundary is the wrong one. */
			cur->ptrace_in_syscall = 0;
			/* The watchpoints referred to addresses in an image
			 * that is gone.  Keeping them would trap on whatever
			 * the new image happens to put there, and if this exec
			 * gained privilege they would be a window into a
			 * program the tracer is no longer entitled to watch. */
			task_debugreg_clear(cur);
			cur->syscall_rip = entry_point;
			cur->syscall_rsp = new_stack_ptr;
			cur->syscall_rflags = new_rflags;
			cur->syscall_rbp = 0;
			cur->syscall_rbx = 0;
			cur->syscall_rax = 0;
			cur->syscall_r12 = 0;
			cur->syscall_r13 = 0;
			cur->syscall_r14 = 0;
			cur->syscall_r15 = 0;
			cur->syscall_rdi = 0;
			cur->syscall_rsi = 0;
			cur->syscall_rdx = 0;
			cur->syscall_r8 = 0;
			cur->syscall_r9 = 0;
			cur->syscall_r10 = 0;
			cur->syscall_regs_valid = 1;

			/* The executable path, BEFORE the stop below and not
			 * after it.
			 *
			 * This is the same statement the register block above
			 * makes -- the old image is gone -- and it has to be
			 * made in the same place, because the stop is where a
			 * debugger asks.  Set afterwards, PTRACE_GETEXECPATH at
			 * an exec stop answered with the PREVIOUS program, and
			 * a debugger took it at its word: gdb announced
			 * "process N is executing new program: /bin/bash" for a
			 * process about to execute something else entirely, then
			 * loaded the wrong symbols for it.  Off by exactly one
			 * exec, which is the hardest kind of wrong to notice --
			 * the answer is always a real program that the process
			 * really did run.
			 *
			 * Absolute, resolved against the working directory this
			 * exec started from: a debugger that attaches later has
			 * a different one, and a relative path would name a
			 * different file (or none) by then. */
			{
				char full[256];
				const char *base = (cur->cwd[0] != 0) ?
							   cur->cwd :
							   "/";

				if (normalize_path(base, kpath, full,
						   sizeof(full)) == 0) {
					int i;

					for (i = 0; i < 255 && full[i]; i++)
						cur->exe_path[i] = full[i];
					cur->exe_path[i] = '\0';
				} else {
					cur->exe_path[0] = '\0';
				}
			}

			if (cur->tracer_pid != 0)
				task_ptrace_stop(cur, SIGTRAP,
						 PTRACE_EVENT_EXEC, 0);

			/* Re-read after the stop: a tracer may have moved the
			 * entry point or the stack with SETREGS while we were
			 * parked, and the jump below is the only thing that
			 * consults them. */
			entry_point = cur->syscall_rip;
			new_stack_ptr = cur->syscall_rsp;
			new_rflags = cur->syscall_rflags;
		}
	}

	// Set task comm from basename of path (or argv[0])
	{
		task_t *cur = sched_current();
		if (cur) {
			/* exe_path is NOT set here.  It is set above, before the
			 * exec stop, because that is where a debugger reads it;
			 * see the comment there. */
			const char *src = kpath;
			// Use basename
			const char *p = src;
			while (*p) {
				if (*p == '/')
					src = p + 1;
				p++;
			}
			int i;
			for (i = 0; i < 255 && src[i]; i++)
				cur->comm[i] = src[i];
			cur->comm[i] = '\0';
			// Build cmdline from argv (space-separated)
			int pos = 0;
			if (kargv) {
				for (int a = 0; kargv[a] && pos < 1023; a++) {
					if (a > 0 && pos < 1023)
						cur->cmdline[pos++] = ' ';
					for (int c = 0;
					     kargv[a][c] && pos < 1023; c++)
						cur->cmdline[pos++] =
							kargv[a][c];
				}
			}
			cur->cmdline[pos] = '\0';
			// Build environ from envp (space-separated)
			pos = 0;
			if (kenvp) {
				for (int a = 0; kenvp[a] && pos < 2047; a++) {
					if (a > 0 && pos < 2047)
						cur->environ[pos++] = ' ';
					for (int c = 0;
					     kenvp[a][c] && pos < 2047; c++)
						cur->environ[pos++] =
							kenvp[a][c];
				}
			}
			cur->environ[pos] = '\0';
		}
	}

	free_user_string_array(kenvp);
	free_user_string_array(kargv);
	kfree(kpath);

	// Success! Jump to the new program
	// We need to return to userspace at the new entry point with the new stack
	// Load the new FS base (TLS / stack canary) before entering user space.
	task_load_tls(sched_current());
	// Use inline assembly to set up IRET frame and jump
	__asm__ volatile(
		"cli\n\t"
		// Set up IRET frame on current stack
		"push $0x1B\n\t" // SS (user data segment)
		"push %0\n\t" // RSP (new user stack)
		"push %2\n\t" // RFLAGS (see new_rflags above)
		"push $0x23\n\t" // CS (user code segment)
		"push %1\n\t" // RIP (entry point)
		// Clear registers for clean start
		"xor %%rax, %%rax\n\t"
		"xor %%rbx, %%rbx\n\t"
		"xor %%rcx, %%rcx\n\t"
		"xor %%rdx, %%rdx\n\t"
		"xor %%rsi, %%rsi\n\t"
		"xor %%rdi, %%rdi\n\t"
		"xor %%rbp, %%rbp\n\t"
		"xor %%r8, %%r8\n\t"
		"xor %%r9, %%r9\n\t"
		"xor %%r10, %%r10\n\t"
		"xor %%r11, %%r11\n\t"
		"xor %%r12, %%r12\n\t"
		"xor %%r13, %%r13\n\t"
		"xor %%r14, %%r14\n\t"
		"xor %%r15, %%r15\n\t"
		"iretq\n\t"
		:
		: "r"(new_stack_ptr), "r"(entry_point), "r"(new_rflags)
		: "memory");

	// Should never reach here
	__builtin_unreachable();

out_err:
	free_user_string_array(kenvp);
	free_user_string_array(kargv);
	kfree(kpath);
	return ret;
}

