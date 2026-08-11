// LikeOS-64 Scheduling and SMP syscall wrappers
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "syscall.h"


// SYS_YIELD - voluntarily yield the CPU
int sched_yield(void)
{
	long ret = syscall0(SYS_YIELD);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// SYS_DEBUG_DUMP - root-only: dump kernel diagnostic tables to the active tty.
// Returns 0 on success, -1/errno on failure (EPERM if not root).
int debug_dump(void)
{
	long ret = syscall0(SYS_DEBUG_DUMP);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// SYS_GETTID - get current thread ID
pid_t gettid(void)
{
	return (pid_t)syscall0(SYS_GETTID);
}

// SYS_VFORK - create child sharing parent's address space
pid_t vfork(void)
{
	long ret = syscall0(SYS_VFORK);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (pid_t)ret;
}

// SYS_EXIT_GROUP - exit all threads in process
void exit_group(int status)
{
	syscall1(SYS_EXIT_GROUP, status);
	// Never returns
	while (1) {
		__asm__ volatile("hlt");
	}
}

// SYS_SCHED_SETAFFINITY - set CPU affinity mask
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask)
{
	long ret = syscall3(SYS_SCHED_SETAFFINITY, pid, cpusetsize, (long)mask);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// SYS_SCHED_GETAFFINITY - get CPU affinity mask
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
{
	long ret = syscall3(SYS_SCHED_GETAFFINITY, pid, cpusetsize, (long)mask);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// SYS_SCHED_SETSCHEDULER - set scheduling policy and parameters
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
	long ret = syscall3(SYS_SCHED_SETSCHEDULER, pid, policy, (long)param);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// SYS_SCHED_GETSCHEDULER - get scheduling policy
int sched_getscheduler(pid_t pid)
{
	long ret = syscall1(SYS_SCHED_GETSCHEDULER, pid);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

// SYS_SCHED_SETPARAM - set scheduling parameters
int sched_setparam(pid_t pid, const struct sched_param *param)
{
	long ret = syscall2(SYS_SCHED_SETPARAM, pid, (long)param);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// SYS_SCHED_GETPARAM - get scheduling parameters
int sched_getparam(pid_t pid, struct sched_param *param)
{
	long ret = syscall2(SYS_SCHED_GETPARAM, pid, (long)param);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// SYS_SCHED_GET_PRIORITY_MAX - get maximum priority for policy
int sched_get_priority_max(int policy)
{
	long ret = syscall1(SYS_SCHED_GET_PRIORITY_MAX, policy);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

// SYS_SCHED_GET_PRIORITY_MIN - get minimum priority for policy
int sched_get_priority_min(int policy)
{
	long ret = syscall1(SYS_SCHED_GET_PRIORITY_MIN, policy);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

// SYS_SCHED_RR_GET_INTERVAL - get round-robin time quantum
int sched_rr_get_interval(pid_t pid, struct timespec *tp)
{
	long ret = syscall2(SYS_SCHED_RR_GET_INTERVAL, pid, (long)tp);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// Futex operations
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

// SYS_FUTEX - fast userspace mutex operations
int futex_wait(volatile int *uaddr, int val, const struct timespec *timeout)
{
	long ret = syscall4(SYS_FUTEX, (long)uaddr, FUTEX_WAIT, val,
			    (long)timeout);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

/*
 * The absolute-deadline form of futex_wait.
 *
 * SYS_FUTEX takes a RELATIVE timeout; POSIX states every deadline it has as an
 * ABSOLUTE time.  Converting between the two is the caller's job, and callers
 * that skipped it (pthread_cond_timedwait and pthread_mutex_timedlock both did)
 * handed the kernel an epoch timestamp as a duration -- about 1.8e9 seconds,
 * which is not a long timeout but an infinite one.  Neither function could ever
 * report ETIMEDOUT.
 *
 * The remaining time is recomputed on every call rather than once, which is
 * also what makes a spurious wake harmless: the wait shrinks towards the
 * deadline instead of restarting from it.  An already-expired deadline waits
 * not at all, as POSIX requires.
 *
 * `abstime' is against CLOCK_REALTIME, the clock POSIX names for all of these
 * interfaces.  NULL waits indefinitely, exactly as futex_wait(..., NULL) does.
 */
int __futex_wait_until_clock(volatile int *uaddr, int val,
			     const struct timespec *abstime, int clock_id)
{
	struct timespec now, rel;

	if (!abstime)
		return futex_wait(uaddr, val, NULL);

	if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L) {
		errno = EINVAL;
		return -1;
	}

	/* Against the clock the CALLER named.  Reading the wall clock for a
	 * deadline measured since boot -- which is what CLOCK_MONOTONIC gives
	 * -- makes the subtraction below hugely negative, so the wait expires
	 * on the spot.  Every timed wait in a program that selects
	 * CLOCK_MONOTONIC then returns ETIMEDOUT immediately. */
	clock_gettime(clock_id, &now);
	rel.tv_sec = abstime->tv_sec - now.tv_sec;
	rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
	if (rel.tv_nsec < 0) {
		rel.tv_nsec += 1000000000L;
		rel.tv_sec--;
	}
	if (rel.tv_sec < 0) {
		errno = ETIMEDOUT;
		return -1;
	}

	return futex_wait(uaddr, val, &rel);
}

int __futex_wait_until(volatile int *uaddr, int val,
		       const struct timespec *abstime)
{
	struct timespec now, rel;

	if (!abstime)
		return futex_wait(uaddr, val, NULL);

	if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L) {
		errno = EINVAL;
		return -1;
	}

	clock_gettime(CLOCK_REALTIME, &now);
	rel.tv_sec = abstime->tv_sec - now.tv_sec;
	rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
	if (rel.tv_nsec < 0) {
		rel.tv_nsec += 1000000000L;
		rel.tv_sec--;
	}
	if (rel.tv_sec < 0) {
		errno = ETIMEDOUT;
		return -1;
	}

	return futex_wait(uaddr, val, &rel);
}

int futex_wake(volatile int *uaddr, int count)
{
	long ret = syscall3(SYS_FUTEX, (long)uaddr, FUTEX_WAKE, count);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

// SYS_SET_TID_ADDRESS - set thread exit notification pointer
int set_tid_address(int *tidptr)
{
	long ret = syscall1(SYS_SET_TID_ADDRESS, (long)tidptr);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return (int)ret;
}

// SYS_SET_ROBUST_LIST - set robust futex list head
int set_robust_list(void *head, size_t len)
{
	long ret = syscall2(SYS_SET_ROBUST_LIST, (long)head, len);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// SYS_GET_ROBUST_LIST - get robust futex list head
int get_robust_list(pid_t pid, void **head_ptr, size_t *len_ptr)
{
	long ret = syscall3(SYS_GET_ROBUST_LIST, pid, (long)head_ptr,
			    (long)len_ptr);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}

// arch_prctl codes
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

// SYS_ARCH_PRCTL - set/get architecture-specific thread state (TLS)
int arch_prctl(int code, unsigned long addr)
{
	long ret = syscall2(SYS_ARCH_PRCTL, code, (long)addr);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}
	return 0;
}
