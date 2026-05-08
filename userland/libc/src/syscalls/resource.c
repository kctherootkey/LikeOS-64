/*
 * resource.c - getrusage / getrlimit / setrlimit wrappers.
 *
 * SYS_GETRUSAGE returns zeros today; the rlimit pair are pure userland
 * stubs reporting "no limit" since the kernel does not enforce per-task
 * resource limits.
 */
#include "../../include/sys/resource.h"
#include "../../include/string.h"
#include "../../include/errno.h"
#include "syscall.h"

int getrusage(int who, struct rusage* usage) {
    if (!usage) { errno = EFAULT; return -1; }
    long ret = syscall2(SYS_GETRUSAGE, who, (long)usage);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int getrlimit(int resource, struct rlimit* rlim) {
    (void)resource;
    if (!rlim) { errno = EFAULT; return -1; }
    rlim->rlim_cur = RLIM_INFINITY;
    rlim->rlim_max = RLIM_INFINITY;
    return 0;
}

int setrlimit(int resource, const struct rlimit* rlim) {
    (void)resource; (void)rlim;
    return 0;
}
