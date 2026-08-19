// LikeOS-64 System Call Handler
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/percpu.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/syscalls.h>

// Main syscall dispatcher (inner function)
static int64_t syscall_handler_inner(uint64_t num, uint64_t a1, uint64_t a2,
				     uint64_t a3, uint64_t a4, uint64_t a5,
				     uint64_t a6)
{
	/* All syscalls run in process context with IRQs enabled — any path
     * that allocates, blocks on I/O, or sleeps must be reachable.  If
     * we ever enter with IRQs off it means a kernel caller bypassed the
     * syscall entry stub; almost every syscall would deadlock. */
	WARN_ON_ONCE(irqs_disabled());
	switch (num) {
	case SYS_READ:
		return sys_read(a1, a2, a3);

	case SYS_WRITE:
		return sys_write(a1, a2, a3);

	case SYS_OPEN:
		return sys_open(a1, a2, a3);

	case SYS_CLOSE:
		return sys_close(a1);

	case SYS_LSEEK:
		return sys_lseek(a1, (int64_t)a2, a3);

	case SYS_MMAP:
		return sys_mmap(a1, a2, a3, a4, a5, a6);

	case SYS_MUNMAP:
		return sys_munmap(a1, a2);

	case SYS_BRK:
		return sys_brk(a1);

	case SYS_GETPID:
		return sys_getpid();

	case SYS_FORK:
		return sys_fork();

	case SYS_WAIT4:
		return sys_waitpid((int64_t)a1, a2, a3, a4);

	case SYS_GETPPID:
		return sys_getppid();

	case SYS_EXECVE:
		return sys_execve(a1, a2, a3);

	case SYS_DUP:
		return sys_dup(a1);

	case SYS_DUP2:
		return sys_dup2(a1, a2);

	case SYS_EXIT:
		sys_exit(a1);
		// Never returns

	case SYS_PIPE:
		return sys_pipe(a1);

	case SYS_YIELD:
		return sys_yield();

	case SYS_STAT:
		return sys_stat(a1, a2);

	case SYS_LSTAT:
		return sys_lstat(a1, a2);

	case SYS_FSTAT:
		return sys_fstat(a1, a2);

	case SYS_ACCESS:
		return sys_access(a1, a2);

	case SYS_CHDIR:
		return sys_chdir(a1);

	case SYS_SHMGET:
		return sys_shmget(a1, a2, a3);
	case SYS_SHMAT:
		return sys_shmat(a1, a2, a3);
	case SYS_SHMDT:
		return sys_shmdt(a1);
	case SYS_SHMCTL:
		return sys_shmctl(a1, a2, a3);

	case SYS_CHROOT:
		return sys_chroot(a1);

	case SYS_GETCWD:
		return sys_getcwd(a1, a2);

	case SYS_UMASK:
		return sys_umask(a1);

	case SYS_GETUID:
		return sys_getuid();

	case SYS_GETGID:
		return sys_getgid();

	case SYS_GETEUID:
		return sys_geteuid();

	case SYS_GETEGID:
		return sys_getegid();

	case SYS_SETUID:
		return sys_setuid(a1);

	case SYS_SETGID:
		return sys_setgid(a1);

	case SYS_SETEUID:
		return sys_seteuid(a1);

	case SYS_SETEGID:
		return sys_setegid(a1);

	case SYS_GETGROUPS:
		return sys_getgroups(a1, a2);

	case SYS_SETGROUPS:
		return sys_setgroups(a1, a2);

	case SYS_SETRESUID:
		return sys_setresuid(a1, a2, a3);
	case SYS_GETRESUID:
		return sys_getresuid(a1, a2, a3);
	case SYS_SETRESGID:
		return sys_setresgid(a1, a2, a3);
	case SYS_GETRESGID:
		return sys_getresgid(a1, a2, a3);
	case SYS_SETXATTR:
		return sys_setxattr(a1, a2, a3, a4, a5);
	case SYS_GETXATTR:
		return sys_getxattr(a1, a2, a3, a4, a5);
	case SYS_LISTXATTR:
		return sys_listxattr(a1, a2, a3, a4);
	case SYS_REMOVEXATTR:
		return sys_removexattr(a1, a2, a3);
	case SYS_FSETXATTR:
		return sys_fsetxattr(a1, a2, a3, a4, a5);
	case SYS_FGETXATTR:
		return sys_fgetxattr(a1, a2, a3, a4);
	case SYS_FLISTXATTR:
		return sys_flistxattr(a1, a2, a3);
	case SYS_FREMOVEXATTR:
		return sys_fremovexattr(a1, a2);

	case SYS_DEBUG_DUMP:
		return sys_debug_dump();

	case SYS_GETHOSTNAME:
		return sys_gethostname(a1, a2);

	case SYS_UNAME:
		return sys_uname(a1);

	case SYS_TIME:
		return sys_time(a1);

	case SYS_GETTIMEOFDAY:
		return sys_gettimeofday(a1, a2);

	case SYS_SETTIMEOFDAY:
		return sys_settimeofday(a1, a2);

	case SYS_FSYNC:
		return sys_fsync(a1);

	case SYS_FTRUNCATE:
		return sys_ftruncate(a1, a2);

	case SYS_FCNTL:
		return sys_fcntl(a1, a2, a3);

	case SYS_IOCTL:
		return sys_ioctl(a1, a2, a3);

	case SYS_SETPGID:
		return sys_setpgid(a1, a2);

	case SYS_GETPGRP:
		return sys_getpgrp();

	case SYS_TCGETPGRP:
		return sys_tcgetpgrp(a1);

	case SYS_TCSETPGRP:
		return sys_tcsetpgrp(a1, a2);

	case SYS_KILL:
		return sys_kill(a1, a2);

	case SYS_UNLINK:
		return sys_unlink(a1);

	case SYS_RENAME:
		return sys_rename(a1, a2);

	case SYS_MKDIR:
		return sys_mkdir(a1, a2);

	case SYS_RMDIR:
		return sys_rmdir(a1);

	case SYS_LINK:
		return sys_link(a1, a2);

	case SYS_SYMLINK:
		return sys_symlink(a1, a2);

	case SYS_READLINK:
		return sys_readlink(a1, a2, a3);

	case SYS_CHMOD:
		return sys_chmod(a1, a2);

	case SYS_FCHMOD:
		return sys_fchmod(a1, a2);

	case SYS_CHOWN:
		return sys_chown(a1, a2, a3);
	case SYS_OPENAT:
		return sys_openat(a1, a2, a3, a4);
	case SYS_UNLINKAT:
		return sys_unlinkat(a1, a2, a3);
	case SYS_FSTATAT:
		return sys_fstatat(a1, a2, a3, a4);
	case SYS_FACCESSAT:
		return sys_faccessat(a1, a2, a3, a4);
	case SYS_GETDENTS64:
		return sys_getdents64(a1, a2, a3);
	case SYS_GETDENTS:
		return sys_getdents(a1, a2, a3);

	case SYS_FCHOWN:
		return sys_fchown(a1, a2, a3);

	case SYS_UTIMENSAT:
		return sys_utimensat(a1, a2, a3, a4);

	case SYS_STATFS:
		return sys_statfs(a1, a2);
	case SYS_FSTATFS:
		return sys_fstatfs(a1, a2);

	// Signal syscalls
	case SYS_RT_SIGACTION:
		return sys_rt_sigaction(a1, a2, a3, a4);
	case SYS_RT_SIGPROCMASK:
		return sys_rt_sigprocmask(a1, a2, a3, a4);
	case SYS_RT_SIGPENDING:
		return sys_rt_sigpending(a1, a2);
	case SYS_RT_SIGTIMEDWAIT:
		return sys_rt_sigtimedwait(a1, a2, a3, a4);
	case SYS_RT_SIGQUEUEINFO:
		return sys_rt_sigqueueinfo(a1, a2, a3);
	case SYS_RT_SIGSUSPEND:
		return sys_rt_sigsuspend(a1, a2);
	case SYS_RT_SIGRETURN:
		return sys_rt_sigreturn();
	case SYS_SIGALTSTACK:
		return sys_sigaltstack(a1, a2);
	case SYS_TKILL:
		return sys_tkill(a1, a2);
	case SYS_TGKILL:
		return sys_tgkill(a1, a2, a3);
	case SYS_ALARM:
		return sys_alarm(a1);
	case SYS_SETITIMER:
		return sys_setitimer(a1, a2, a3);
	case SYS_GETITIMER:
		return sys_getitimer(a1, a2);
	case SYS_TIMER_CREATE:
		return sys_timer_create(a1, a2, a3);
	case SYS_TIMER_SETTIME:
		return sys_timer_settime(a1, a2, a3, a4);
	case SYS_TIMER_GETTIME:
		return sys_timer_gettime(a1, a2);
	case SYS_TIMER_GETOVERRUN:
		return sys_timer_getoverrun(a1);
	case SYS_TIMER_DELETE:
		return sys_timer_delete(a1);
	case SYS_SIGNALFD:
		return sys_signalfd(a1, a2, a3);
	case SYS_PAUSE:
		return sys_pause();
	case SYS_NANOSLEEP:
		return sys_nanosleep(a1, a2);
	case SYS_CLOCK_GETTIME:
		return sys_clock_gettime(a1, a2);
	case SYS_CLOCK_GETRES:
		return sys_clock_getres(a1, a2);

	// SMP/Threading syscalls
	case SYS_CLONE:
		return sys_clone(a1, a2, a3, a4, a5);
	case SYS_VFORK:
		return sys_vfork();
	case SYS_EXIT_GROUP:
		sys_exit_group(a1);
		return 0; // Never reached
	case SYS_GETTID:
		return sys_gettid();
	case SYS_SET_TID_ADDRESS:
		return sys_set_tid_address(a1);
	case SYS_FUTEX:
		return sys_futex(a1, a2, a3, a4, a5, 0);
	case SYS_SET_ROBUST_LIST:
		return sys_set_robust_list(a1, a2);
	case SYS_GET_ROBUST_LIST:
		return sys_get_robust_list(a1, a2, a3);
	case SYS_ARCH_PRCTL:
		return sys_arch_prctl(a1, a2);
	case SYS_SCHED_SETAFFINITY:
		return sys_sched_setaffinity(a1, a2, a3);
	case SYS_SCHED_GETAFFINITY:
		return sys_sched_getaffinity(a1, a2, a3);
	case SYS_SCHED_SETSCHEDULER:
		return sys_sched_setscheduler(a1, a2, a3);
	case SYS_SCHED_GETSCHEDULER:
		return sys_sched_getscheduler(a1);
	case SYS_SCHED_SETPARAM:
		return sys_sched_setparam(a1, a2);
	case SYS_SCHED_GETPARAM:
		return sys_sched_getparam(a1, a2);
	case SYS_SCHED_GET_PRIORITY_MAX:
		return sys_sched_get_priority_max(a1);
	case SYS_SCHED_GET_PRIORITY_MIN:
		return sys_sched_get_priority_min(a1);
	case SYS_SCHED_RR_GET_INTERVAL:
		return sys_sched_rr_get_interval(a1, a2);
	case SYS_MPROTECT:
		return sys_mprotect(a1, a2, a3);

	case SYS_MADVISE:
		return sys_madvise(a1, a2, a3);

	case SYS_REBOOT:
		return sys_reboot(a1, a2, a3, a4);

	case SYS_GETPROCINFO:
		return sys_getprocinfo(a1, a2);

	case SYS_GETPROCMAPS:
		return sys_getprocmaps(a1, a2, a3, a4);

	case SYS_PTRACE:
		return sys_ptrace(a1, a2, a3, a4);

	case SYS_MEMSTATS:
		return sys_memstats(a1, a2);

	case SYS_SYSINFO:
		return sys_sysinfo(a1);

	case SYS_KLOGCTL:
		return sys_klogctl(a1, a2, a3);

	case SYS_SYNC:
		return sys_sync();

	// ====== Socket syscalls ======
	case SYS_SOCKET:
		return sys_socket(a1, a2, a3);

	case SYS_BIND:
		return sys_bind(a1, a2, a3);

	case SYS_LISTEN:
		return sys_listen(a1, a2);

	case SYS_ACCEPT:
		return sys_accept(a1, a2, a3);

	case SYS_CONNECT:
		return sys_connect(a1, a2, a3);

	case SYS_SENDTO:
		return sys_sendto(a1, a2, a3, a4, a5);

	case SYS_RECVFROM:
		return sys_recvfrom(a1, a2, a3, a4, a5);

	case SYS_SEND:
		return sys_send(a1, a2, a3, a4);

	case SYS_RECV:
		return sys_recv(a1, a2, a3, a4);

	case SYS_SHUTDOWN:
		return sys_shutdown(a1, a2);

	case SYS_SETSOCKOPT:
		return sys_setsockopt(a1, a2, a3, a4, a5);

	case SYS_GETSOCKOPT:
		return sys_getsockopt(a1, a2, a3, a4, a5);

	case SYS_GETPEERNAME:
		return sys_getpeername(a1, a2, a3);

	case SYS_GETSOCKNAME:
		return sys_getsockname(a1, a2, a3);

	case SYS_SOCKETPAIR:
		return sys_socketpair(a1, a2, a3, a4);

	case SYS_ACCEPT4:
		return sys_accept4(a1, a2, a3, a4);

	case SYS_SENDMSG:
		return sys_sendmsg(a1, a2, a3);

	case SYS_RECVMSG:
		return sys_recvmsg(a1, a2, a3);

	case SYS_SENDFILE:
		return sys_sendfile(a1, a2, a3, a4);

	case SYS_SELECT:
		return sys_select(a1, a2, a3, a4, a5);

	case SYS_PSELECT6:
		return sys_pselect6(a1, a2, a3, a4, a5, a6);

	case SYS_POLL:
		return sys_poll(a1, a2, a3);

	case SYS_PPOLL:
		return sys_ppoll(a1, a2, a3, a4);

	case SYS_EPOLL_CREATE:
		return sys_epoll_create();

	case SYS_EPOLL_CREATE1:
		return sys_epoll_create1(a1);

	case SYS_EPOLL_CTL:
		return sys_epoll_ctl(a1, a2, a3, a4);

	case SYS_EPOLL_WAIT:
		return sys_epoll_wait(a1, a2, a3, a4);

	case SYS_EPOLL_PWAIT:
		return sys_epoll_wait(a1, a2, a3, a4);

	case SYS_DUP3:
		return sys_dup3(a1, a2, a3);

	case SYS_DNS_RESOLVE:
		return sys_dns_resolve(a1, a2);

	case SYS_SETHOSTNAME:
		return sys_sethostname(a1, a2);

	case SYS_NET_GETINFO:
		return sys_net_getinfo(a1, a2, a3);

	case SYS_DHCP_CONTROL:
		return sys_dhcp_control(a1);

	case SYS_RAW_SEND:
		return sys_raw_send(a1, a2, a3, a4, a5);

	case SYS_RAW_RECV:
		return sys_raw_recv(a1, a2, a3, a4);

	case SYS_DNS_RESOLVE_REVERSE:
		return sys_dns_resolve_reverse(a1, a2, a3);

	case SYS_SET_DNS_SERVER:
		return sys_set_dns_server(a1, a2);

	case SYS_SETSID:
		return sys_setsid();
	case SYS_GETSID:
		return sys_getsid(a1);
	case SYS_GETPGID:
		return sys_getpgid(a1);
	case SYS_GETRUSAGE:
		return sys_getrusage(a1, a2);
	case SYS_READV:
		return sys_readv(a1, a2, a3);
	case SYS_WRITEV:
		return sys_writev(a1, a2, a3);

	case SYS_GETRANDOM:
		return sys_getrandom(a1, a2, a3);

	default:
		return -ENOSYS;
	}
}

// Wrapper that handles signal delivery after syscall
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
			uint64_t a4, uint64_t a5, uint64_t a6)
{
	// CRITICAL: Interrupts are DISABLED when we enter (syscall_entry no longer does sti)
	// This prevents a race where:
	// 1. Task A enters syscall, writes to per-CPU storage
	// 2. Timer fires, preempts to task B
	// 3. Task B makes syscall, overwrites per-CPU storage
	// 4. Task A resumes, reads corrupted values from per-CPU
	//
	// We snapshot the per-CPU values to task-local storage before enabling interrupts.
	task_t *cur = sched_current();
	percpu_t *cpu = this_cpu();
	BUG_ON(cpu == NULL);

	/* Track current syscall number for Oops/panic reporting */
	cpu->current_syscall_nr = (int)num;

	if (cur && cur->privilege == TASK_USER) {
		/* The data-segment selectors, before anything else runs.  Only
		 * a debugger asks for them, but the moment of entry is the only
		 * moment at which they are still the program's. */
		task_capture_user_segments(cur);

		// Read from per-CPU storage (set by syscall_entry in assembly)
		cur->syscall_rsp = cpu->syscall_user_rsp;
		cur->syscall_rip = cpu->syscall_saved_user_rip;
		cur->syscall_rflags = cpu->syscall_saved_user_rflags;
		cur->syscall_rbp = cpu->syscall_saved_user_rbp;
		cur->syscall_rbx = cpu->syscall_saved_user_rbx;
		cur->syscall_r12 = cpu->syscall_saved_user_r12;
		cur->syscall_r13 = cpu->syscall_saved_user_r13;
		cur->syscall_r14 = cpu->syscall_saved_user_r14;
		cur->syscall_r15 = cpu->syscall_saved_user_r15;

		/* The argument registers, from the frame syscall_entry left on
		 * the kernel stack.  Copied here for the same reason as
		 * everything above: the per-CPU slot describes whoever entered
		 * a syscall on this CPU last, and once interrupts come back on
		 * that may no longer be this task.
		 *
		 * Nothing else preserves these -- a syscall is allowed to
		 * clobber them -- so without this a tracee stopped inside a
		 * syscall has no arguments to show and no way to say so. */
		const syscall_user_frame_t *uf =
			(const syscall_user_frame_t *)cpu->syscall_user_frame;

		if (uf) {
			cur->syscall_rdi = uf->rdi;
			cur->syscall_rsi = uf->rsi;
			cur->syscall_rdx = uf->rdx;
			cur->syscall_r8 = uf->r8;
			cur->syscall_r9 = uf->r9;
			cur->syscall_r10 = uf->r10;
			cur->syscall_regs_valid = 1;
			/* Kept as well as copied: the values above can be read
			 * back, but only the frame itself can be CHANGED in a
			 * way the return path will honour. */
			cur->syscall_frame = (syscall_user_frame_t *)uf;
		} else {
			cur->syscall_regs_valid = 0;
			cur->syscall_frame = NULL;
		}
	}

	// NOW enable interrupts - per-CPU values are safely copied to task struct
	__asm__ volatile("sti" ::: "memory");

	/* Syscall-entry stop.
	 *
	 * After the register snapshot above, so a tracer inspecting the stop
	 * sees the call's arguments; and after interrupts are back on, because
	 * stopping means scheduling away.  A tracer may change the arguments
	 * here -- that is the point of stopping before the work is done -- so
	 * the values actually passed to the handler are re-read afterwards. */
	if (cur && cur->tracer_pid != 0 && cur->ptrace_syscall_trace &&
	    !cur->ptrace_in_syscall) {
		cur->ptrace_in_syscall = 1;
		task_ptrace_stop(cur, SIGTRAP, PTRACE_EVENT_SYSCALL_ENTRY, num);

		/* Re-read: SETREGS during the stop is how a tracer redirects or
		 * rewrites a call, and it writes the frame these came from. */
		if (cur->syscall_frame) {
			num = cur->syscall_frame->rax;
			a1 = cur->syscall_frame->rdi;
			a2 = cur->syscall_frame->rsi;
			a3 = cur->syscall_frame->rdx;
			a4 = cur->syscall_frame->r10;
			a5 = cur->syscall_frame->r8;
			a6 = cur->syscall_frame->r9;
		}
	}

	int64_t ret = syscall_handler_inner(num, a1, a2, a3, a4, a5, a6);

	/* Syscall-exit stop, with the return value already in place so a tracer
	 * can read it -- and change it, which is the only way to make a syscall
	 * appear to have failed or succeeded differently than it did. */
	if (cur && cur->tracer_pid != 0 && cur->ptrace_syscall_trace &&
	    cur->ptrace_in_syscall && !cur->has_exited) {
		cur->ptrace_in_syscall = 0;
		cur->syscall_rax = (uint64_t)ret;
		if (cur->syscall_frame)
			cur->syscall_frame->rax = (uint64_t)ret;

		task_ptrace_stop(cur, SIGTRAP, PTRACE_EVENT_SYSCALL_EXIT, num);

		if (cur->syscall_frame)
			ret = (int64_t)cur->syscall_frame->rax;
	}

	/* Release the socket this syscall held, if it resolved one.  Here
	 * rather than in each arm: the arms return from many places, and a
	 * missed release permanently claims the socket. */
	if (cur && cur->syscall_unix_ref) {
		struct unix_socket *held = cur->syscall_unix_ref;

		cur->syscall_unix_ref = NULL;
		unix_sock_put_ref(held);
	}

	// Check for pending signals before returning to userspace.
	// Skip for:
	//   * SYS_EXIT       — task is already being torn down.
	//   * SYS_RT_SIGRETURN — just restored a signal-frame context; another
	//                        delivery here would clobber it.
	//   * has_exited / TASK_ZOMBIE — task has already died inside the
	//     syscall (e.g. SIGKILL from another CPU mid-syscall, or the
	//     syscall handler called sched_mark_task_exited).  Calling
	//     signal_deliver on a zombie tripped a WARN_ON at signal.c:619
	//     and the subsequent code paths there are racy on a half-torn-down
	//     task.  Just fall through; sched_schedule below will pick the
	//     next task and we'll never return to userspace.
	if (num != SYS_EXIT && num != SYS_RT_SIGRETURN) {
		cur = sched_current(); // Re-fetch in case of fork

		/* Park here if the process is in a trace stop.  Checked
		 * independently of signal_pending: a thread with nothing
		 * pending still has to stop when its process does. */
		if (cur && cur->privilege == TASK_USER && !cur->has_exited &&
		    cur->state != TASK_ZOMBIE && cur->ptrace_group_stop)
			task_ptrace_group_park(cur);

		cur = sched_current();
		if (cur && cur->privilege == TASK_USER && !cur->has_exited &&
		    cur->state != TASK_ZOMBIE && signal_pending(cur)) {
			// Save syscall return value so sigreturn can restore it
			cur->syscall_rax = (uint64_t)ret;
			signal_deliver(cur);
			// Check if signal_deliver terminated the task (e.g., SIG_DFL for SIGTERM)
			if (cur->has_exited || cur->state == TASK_ZOMBIE) {
				sched_schedule();
				// Should not return here
			}
		}
	}

	/* ppoll()/pselect() leave their temporary mask installed across the
	 * delivery above so the signal the caller unblocked can actually run
	 * its handler.  Put the caller's mask back now — unless a handler was
	 * set up, in which case signal_setup_frame already took ownership and
	 * sigreturn restores it after the handler returns. */
	{
		task_t *mcur = sched_current();
		if (mcur)
			poll_sigmask_restore_pending(mcur);
	}

	/* Clear syscall tracking on return.  Re-derive the percpu pointer:
	 * the syscall may have blocked and this task may have been resumed on
	 * a DIFFERENT CPU — `cpu` from function entry would then point at the
	 * old CPU and clobber ITS current_syscall_nr mid-syscall. */
	this_cpu()->current_syscall_nr = -1;

	/* A task that DIED inside this syscall must never sysret to user mode.
	 * sched_mark_task_exited() has already closed its descriptors and
	 * released its demand-paging region table, so the moment it executes
	 * user code again the first not-yet-paged-in text page faults with no
	 * region backing it — a bogus SIGSEGV report (with an empty region
	 * list) for a process that was already dead.  That is exactly what
	 * `kill -TERM $$` produced: sys_kill → SIG_DFL_TERM on the caller
	 * itself → marked exited, and the signal-delivery block above is
	 * skipped precisely BECAUSE has_exited is set, so control fell
	 * straight through to `return ret`.  Park here instead and let the
	 * scheduler take us off this CPU for good.
	 *
	 * IRQs stay ENABLED in the retry loop (`sti; hlt`, same as sys_exit):
	 * a CPU halted with IRQs off can no longer ack TLB-shootdown IPIs and
	 * wedges every other CPU spinning in smp_tlb_shootdown_sync(). */
	cur = sched_current();
	if (cur && cur->privilege == TASK_USER &&
	    (cur->has_exited || cur->state == TASK_ZOMBIE)) {
		/* This is where the threads of an exiting group actually end
		 * up: signalling a thread never tears it down in place, so
		 * each one unwinds its own syscall and arrives here, in its
		 * own context with interrupts on -- the right place to give
		 * its address space back. */
		sched_exit_park();
	}

	/* The frame is about to be popped off the kernel stack, so stop
	 * pointing at it.  Left set, it would name whatever the next syscall
	 * puts at those addresses -- and a tracer that read or wrote registers
	 * through it would be reading, or corrupting, an unrelated frame. */
	if (cur)
		cur->syscall_frame = NULL;

	return ret;
}
