/*
 * resource.c - getrusage / getrlimit / setrlimit wrappers.
 *
 * SYS_GETRUSAGE returns zeros today; the rlimit pair are pure userland stubs,
 * since the kernel does not enforce per-task resource limits.  RLIMIT_NOFILE
 * is the exception -- the fd table is a fixed size, so that one is answered
 * honestly.
 */
#include <sys/resource.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "syscall.h"

int getrusage(int who, struct rusage *usage)
{
	if (!usage) {
		errno = EFAULT;
		return -1;
	}
	long ret = syscall2(SYS_GETRUSAGE, who, (long)usage);
	if (ret < 0) {
		errno = (int)-ret;
		return -1;
	}
	return 0;
}

int getrlimit(int resource, struct rlimit *rlim)
{
	if (!rlim) {
		errno = EFAULT;
		return -1;
	}
	/*
	 * RLIMIT_NOFILE is the one limit that is real here, and reporting
	 * "infinite" for it is worse than useless: the descriptor table IS
	 * bounded (OPEN_MAX, matching the kernel's TASK_MAX_FDS), and the
	 * standard idiom for closing inherited descriptors is
	 *
	 *	getrlimit(RLIMIT_NOFILE, &rl);
	 *	for (fd = 3; fd < rl.rlim_cur; fd++) close(fd);
	 *
	 * which against RLIM_INFINITY does not terminate in any useful time.
	 * Everything else genuinely is unlimited: the kernel enforces no
	 * per-task resource limits.
	 */
	if (resource == RLIMIT_NOFILE) {
		rlim->rlim_cur = OPEN_MAX;
		rlim->rlim_max = OPEN_MAX;
		return 0;
	}
	rlim->rlim_cur = RLIM_INFINITY;
	rlim->rlim_max = RLIM_INFINITY;
	return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
	(void)resource;
	(void)rlim;
	return 0;
}
