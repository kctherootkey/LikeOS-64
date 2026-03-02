// LikeOS-64 Scheduling and SMP syscall wrappers
#include "../../include/sched.h"
#include "../../include/unistd.h"
#include "../../include/errno.h"
#include "syscall.h"

extern int errno;

// SYS_YIELD - voluntarily yield the CPU
int sched_yield(void) {
    long ret = syscall0(SYS_YIELD);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// SYS_GETTID - get current thread ID
pid_t gettid(void) {
    return (pid_t)syscall0(SYS_GETTID);
}

// SYS_VFORK - create child sharing parent's address space
pid_t vfork(void) {
    long ret = syscall0(SYS_VFORK);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return (pid_t)ret;
}

// SYS_EXIT_GROUP - exit all threads in process
void exit_group(int status) {
    syscall1(SYS_EXIT_GROUP, status);
    // Never returns
    while (1) { __asm__ volatile("hlt"); }
}

// SYS_SCHED_SETAFFINITY - set CPU affinity mask
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t* mask) {
    long ret = syscall3(SYS_SCHED_SETAFFINITY, pid, cpusetsize, (long)mask);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// SYS_SCHED_GETAFFINITY - get CPU affinity mask
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask) {
    long ret = syscall3(SYS_SCHED_GETAFFINITY, pid, cpusetsize, (long)mask);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// SYS_SCHED_SETSCHEDULER - set scheduling policy and parameters
int sched_setscheduler(pid_t pid, int policy, const struct sched_param* param) {
    long ret = syscall3(SYS_SCHED_SETSCHEDULER, pid, policy, (long)param);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// SYS_SCHED_GETSCHEDULER - get scheduling policy
int sched_getscheduler(pid_t pid) {
    long ret = syscall1(SYS_SCHED_GETSCHEDULER, pid);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return (int)ret;
}

// SYS_SCHED_SETPARAM - set scheduling parameters
int sched_setparam(pid_t pid, const struct sched_param* param) {
    long ret = syscall2(SYS_SCHED_SETPARAM, pid, (long)param);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// SYS_SCHED_GETPARAM - get scheduling parameters
int sched_getparam(pid_t pid, struct sched_param* param) {
    long ret = syscall2(SYS_SCHED_GETPARAM, pid, (long)param);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// SYS_SCHED_GET_PRIORITY_MAX - get maximum priority for policy
int sched_get_priority_max(int policy) {
    long ret = syscall1(SYS_SCHED_GET_PRIORITY_MAX, policy);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return (int)ret;
}

// SYS_SCHED_GET_PRIORITY_MIN - get minimum priority for policy
int sched_get_priority_min(int policy) {
    long ret = syscall1(SYS_SCHED_GET_PRIORITY_MIN, policy);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return (int)ret;
}

// SYS_SCHED_RR_GET_INTERVAL - get round-robin time quantum
int sched_rr_get_interval(pid_t pid, struct timespec* tp) {
    long ret = syscall2(SYS_SCHED_RR_GET_INTERVAL, pid, (long)tp);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// Futex operations
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_PRIVATE_FLAG  128

// SYS_FUTEX - fast userspace mutex operations
int futex_wait(int* uaddr, int val, const struct timespec* timeout) {
    long ret = syscall4(SYS_FUTEX, (long)uaddr, FUTEX_WAIT, val, (long)timeout);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

int futex_wake(int* uaddr, int count) {
    long ret = syscall3(SYS_FUTEX, (long)uaddr, FUTEX_WAKE, count);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return (int)ret;
}

// SYS_SET_TID_ADDRESS - set thread exit notification pointer
int set_tid_address(int* tidptr) {
    long ret = syscall1(SYS_SET_TID_ADDRESS, (long)tidptr);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return (int)ret;
}

// SYS_SET_ROBUST_LIST - set robust futex list head
int set_robust_list(void* head, size_t len) {
    long ret = syscall2(SYS_SET_ROBUST_LIST, (long)head, len);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// SYS_GET_ROBUST_LIST - get robust futex list head
int get_robust_list(pid_t pid, void** head_ptr, size_t* len_ptr) {
    long ret = syscall3(SYS_GET_ROBUST_LIST, pid, (long)head_ptr, (long)len_ptr);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}

// arch_prctl codes
#define ARCH_SET_GS     0x1001
#define ARCH_SET_FS     0x1002
#define ARCH_GET_FS     0x1003
#define ARCH_GET_GS     0x1004

// SYS_ARCH_PRCTL - set/get architecture-specific thread state (TLS)
int arch_prctl(int code, unsigned long addr) {
    long ret = syscall2(SYS_ARCH_PRCTL, code, (long)addr);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return 0;
}
