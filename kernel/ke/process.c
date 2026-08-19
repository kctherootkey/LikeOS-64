// LikeOS-64 -- process identity, sessions, process groups and rusage.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>
#include <kernel/uapi/rusage.h>


int64_t sys_setpgid(uint64_t pid, uint64_t pgid)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (pid == 0) {
		pid = (uint64_t)cur->id;
	}
	if (pgid == 0) {
		pgid = pid;
	}
	task_t *t = sched_find_task_by_id((uint32_t)pid);
	if (!t) {
		return -ESRCH;
	}
	t->pgid = (int)pgid;
	return 0;
}


int64_t sys_getpgrp(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	return cur->pgid;
}


int64_t sys_tcgetpgrp(uint64_t fd)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	vfs_file_t *file = NULL;
	if (fd < TASK_MAX_FDS) {
		file = task_fds(cur)[fd];
	}
	tty_t *tty = NULL;
	if (task_fd_is_console(cur, fd)) {
		tty = cur->ctty ? cur->ctty : tty_get_console();
	}
	if (file) {
		uintptr_t mk = (uintptr_t)file;
		if (mk >= 1 && mk <= 3) {
			tty = cur->ctty ? cur->ctty : tty_get_console();
		} else {
			tty = devfs_get_tty(file);
		}
	}
	if (!tty) {
		return -ENOTTY;
	}
	return tty->fg_pgid;
}


int64_t sys_tcsetpgrp(uint64_t fd, uint64_t pgrp)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	vfs_file_t *file = NULL;
	if (fd < TASK_MAX_FDS) {
		file = task_fds(cur)[fd];
	}
	tty_t *tty = NULL;
	if (task_fd_is_console(cur, fd)) {
		tty = cur->ctty ? cur->ctty : tty_get_console();
	}
	if (file) {
		uintptr_t mk = (uintptr_t)file;
		if (mk >= 1 && mk <= 3) {
			tty = cur->ctty ? cur->ctty : tty_get_console();
		} else {
			tty = devfs_get_tty(file);
		}
	}
	if (!tty) {
		return -ENOTTY;
	}
	tty->fg_pgid = (int)pgrp;
	return 0;
}


// POSIX setsid(2) - create a new session.
// On success the calling process becomes session leader, gets a new
// process group, and is detached from any controlling tty.  Returns
// the new session ID (the pid).  Returns -EPERM if the caller is
// already a process-group leader.
int64_t sys_setsid(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (cur->pgid == (int)cur->id) {
		return -EPERM;
	}
	cur->sid = (int)cur->id;
	cur->pgid = (int)cur->id;
	cur->ctty = NULL;
	return cur->id;
}


// POSIX getsid(2) - return session ID of process pid (0 = self).
int64_t sys_getsid(uint64_t pid)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (pid == 0)
		return cur->sid;
	task_t *t = sched_find_task_by_id((uint32_t)pid);
	if (!t)
		return -ESRCH;
	return t->sid;
}


// POSIX getpgid(2) - return process-group ID of process pid (0 = self).
int64_t sys_getpgid(uint64_t pid)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;
	if (pid == 0)
		return cur->pgid;
	task_t *t = sched_find_task_by_id((uint32_t)pid);
	if (!t)
		return -ESRCH;
	return t->pgid;
}

// POSIX getrusage(2) - resource usage.
//
// The CPU times are real: the timer tick charges every task to utime_ticks or
// stime_ticks depending on the privilege level it interrupted (see
// timer_handle_tick), so the accounting already exists and only needs
// reporting.  wait4() has been reporting a dead child's times from the same
// counters all along; this is the same conversion for a live process.
//
// Resolution is one timer tick, so a process that has run for less than a tick
// reads as zero.  That is the honest answer at this sampling rate, not a bug.
//
// The remaining fields stay zero because nothing counts them yet: there is no
// per-task page-fault or resident-set accounting to report.
/*
 * struct rusage, in the layout every other Unix uses on x86-64 and exactly as
 * userspace declares it in <sys/wait.h>: two timevals then fourteen longs.
 *
 * THE FIELDS AND THE SIZE HERE ARE AN ABI CONTRACT WITH THAT HEADER, because
 * neither getrusage(2) nor wait4(2) carries a length -- the kernel copies a
 * fixed-size structure and the two declarations have to agree exactly.  They
 * did not: this was 144 bytes of `ru_pad' against a header declaring 72, and
 * sys_getrusage copied its own sizeof() to the caller, so every getrusage()
 * wrote 72 bytes PAST the end of the caller's structure.
 *
 * That is a kernel buffer overflow into userspace, and it is as bad as it
 * sounds.  It survived so long because the damage lands on whatever the caller
 * keeps after the structure: harmless padding in one program, a saved return
 * address in the next.  gdb was the second kind -- get_run_time() puts its
 * rusage in an 88-byte frame with the return address just past it, so the
 * overflow replaced that return address with a zeroed pad word and the process
 * jumped to 0 on return, with no instruction bytes at RIP and nothing on the
 * stack to say where it came from.
 *
 * The named fields the kernel does not account for yet are reported as zero.
 * They are declared rather than padded so this and the header can be read
 * against each other.
 */


#define K_RUSAGE_SELF 0
#define K_RUSAGE_CHILDREN (-1)

int64_t sys_getrusage(uint64_t who, uint64_t uptr)
{
	if (!uptr)
		return -EFAULT;
	if (!validate_user_ptr(uptr, sizeof(struct k_rusage_compat)))
		return -EFAULT;

	struct k_rusage_compat ru;
	for (size_t i = 0; i < sizeof(ru); i++)
		((uint8_t *)&ru)[i] = 0;

	task_t *cur = sched_current();
	switch ((int)(int32_t)who) {
	case K_RUSAGE_SELF:
		if (cur) {
			ticks_to_timeval(cur->utime_ticks, &ru.ru_utime_sec,
					 &ru.ru_utime_usec);
			ticks_to_timeval(cur->stime_ticks, &ru.ru_stime_sec,
					 &ru.ru_stime_usec);
		}
		break;
	case K_RUSAGE_CHILDREN:
		// Reaped children's times are not accumulated onto the parent,
		// so this is zero rather than wrong.  wait4() reports each
		// child's own usage as it is reaped, which is where a caller
		// can get the real figures today.
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user((void *)uptr, &ru, sizeof(ru)) != 0)
		return -EFAULT;
	return 0;
}


// SYS_GETPPID - get parent process ID
int64_t sys_getppid(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return 0;
	return sched_get_ppid(cur);
}


// SYS_GETPID - get process ID (thread group ID)
// With thread groups, getpid() returns the tgid (thread group leader's ID)
// which is the same for all threads in the process.
int64_t sys_getpid(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -1;
	// Return tgid (thread group ID) which equals id for single-threaded processes
	return cur->tgid;
}


// SYS_GETTID - get thread ID (unique per thread)
int64_t sys_gettid(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -ESRCH;
	return cur->id; // TID is always the unique task ID
}


// SYS_SET_TID_ADDRESS - set address for clear-on-exit notification
int64_t sys_set_tid_address(uint64_t tidptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -ESRCH;

	if (tidptr && validate_user_ptr(tidptr, sizeof(int))) {
		cur->clear_child_tid = (uint64_t *)tidptr;
	} else {
		cur->clear_child_tid = NULL;
	}

	return cur->id;
}


// arch_prctl codes
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

// SYS_ARCH_PRCTL - architecture-specific thread state
int64_t sys_arch_prctl(uint64_t code, uint64_t addr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -ESRCH;

	switch (code) {
	case ARCH_SET_FS: {
		/* Reject any address whose top bit (bit 47) is 1.  A user
             * task may only set FS to USER-space addresses.  Canonical
             * checking alone (allowing top == 0x1FFFF, i.e. kernel half)
             * would let user code point FS at an arbitrary kernel VA,
             * which then poisons every libc stack-canary read
             * (`mov %fs:0x28, %rax`) and any other FS-relative TLS
             * access.  The resulting #PF can land in kernel mode if
             * triggered while the kernel is between SWAPGS / sysret —
             * a wild fault that is almost impossible to reproduce. */
		if (addr != 0 && (addr >> 47) != 0)
			return -EINVAL;
		task_set_fs_base(cur, addr);
		// Apply immediately
		task_load_tls(cur);
		return 0;
	}

	case ARCH_GET_FS:
		if (!validate_user_ptr(addr, sizeof(uint64_t))) {
			return -EFAULT;
		}
		smap_disable();
		*(uint64_t *)addr = cur->fs_base;
		smap_enable();
		return 0;

	case ARCH_SET_GS: {
		/* Reject kernel addresses (top bit set) — same rationale as
             * ARCH_SET_FS above.  Even though task_load_tls today does
             * not push gs_base into MSR_GS_BASE (the kernel manages %gs
             * exclusively for per-CPU data), the field is exposed by
             * ARCH_GET_GS and could be picked up by future code; refuse
             * the bad value at the API boundary. */
		if (addr != 0 && (addr >> 47) != 0)
			return -EINVAL;
		cur->gs_base = addr;
		return 0;
	}

	case ARCH_GET_GS:
		if (!validate_user_ptr(addr, sizeof(uint64_t))) {
			return -EFAULT;
		}
		smap_disable();
		*(uint64_t *)addr = cur->gs_base;
		smap_enable();
		return 0;

	default:
		return -EINVAL;
	}
}

