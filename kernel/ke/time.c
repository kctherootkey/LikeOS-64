// LikeOS-64 -- time of day, clocks, sleeping and interval timers.
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>

typedef struct {
	long tv_sec;
	long tv_usec;
} k_timeval_t;

int64_t sys_time(uint64_t tloc)
{
	uint64_t sec = timer_get_epoch();
	if (tloc && validate_user_ptr(tloc, sizeof(uint64_t))) {
		copy_to_user((void *)tloc, &sec, sizeof(sec));
	}
	return (int64_t)sec;
}

int64_t sys_gettimeofday(uint64_t tv, uint64_t tz)
{
	(void)tz;
	if (!validate_user_ptr(tv, sizeof(k_timeval_t)))
		return -EFAULT;
	k_timeval_t kv;
	// Use timer_get_precise_us() which atomically reads g_ticks and the
	// ACPI PM Timer via a seqlock, eliminating the race where a BSP tick
	// fires between reading the two values and corrupts the sub-tick delta.
	uint64_t total_us = timer_get_precise_us();
	uint64_t boot_epoch = timer_get_boot_epoch();

	kv.tv_sec = (long)(boot_epoch + total_us / 1000000ULL);
	kv.tv_usec = (long)(total_us % 1000000ULL);

	if (copy_to_user((void *)tv, &kv, sizeof(kv)) < 0) {
		return -EFAULT;
	}
	return 0;
}

int64_t sys_settimeofday(uint64_t tv_ptr, uint64_t tz)
{
	(void)tz;
	/* Setting the wall-clock is a system-wide change: privileged only. */
	if (!capable())
		return -EPERM;
	if (!validate_user_ptr(tv_ptr, sizeof(k_timeval_t)))
		return -EFAULT;
	k_timeval_t kv;
	if (copy_from_user(&kv, (const void *)tv_ptr, sizeof(kv)) < 0)
		return -EFAULT;
	/* Set the wall-clock time (adjusts boot_epoch and writes CMOS RTC) */
	timer_set_time((uint64_t)kv.tv_sec);
	return 0;
}

// Ticks -> seconds and microseconds, at whatever rate the timer is running.
void ticks_to_timeval(uint64_t ticks, int64_t *sec, int64_t *usec)
{
	uint32_t freq = timer_get_frequency();
	if (freq == 0)
		freq = 100;
	*sec = (int64_t)(ticks / freq);
	*usec = (int64_t)((ticks % freq) * (1000000 / freq));
}

// Forward declaration for signal functions
extern ktimer_t timer_create_internal(task_t *task, clockid_t clockid,
				      struct k_sigevent *sevp);
extern int timer_settime_internal(ktimer_t timerid, int flags,
				  const struct k_itimerspec *new_value,
				  struct k_itimerspec *old_value);
extern int timer_gettime_internal(ktimer_t timerid,
				  struct k_itimerspec *curr_value);
extern int timer_getoverrun_internal(ktimer_t timerid);
extern int timer_delete_internal(ktimer_t timerid);

// SYS_ALARM - set alarm clock
int64_t sys_alarm(uint64_t seconds)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	uint64_t old_remaining = 0;

	// Calculate remaining time from old alarm
	uint32_t freq = timer_get_frequency();
	if (cur->signals.alarm_ticks > 0) {
		uint64_t now = timer_ticks();
		if (cur->signals.alarm_ticks > now) {
			old_remaining = (cur->signals.alarm_ticks - now) / freq;
		}
	}

	// Set new alarm
	if (seconds > 0) {
		cur->signals.alarm_ticks = timer_ticks() + seconds * freq;
	} else {
		cur->signals.alarm_ticks = 0; // Cancel
	}

	return (int64_t)old_remaining;
}

// SYS_SETITIMER - set interval timer
int64_t sys_setitimer(uint64_t which, uint64_t new_value_ptr,
		      uint64_t old_value_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_itimerval *timer;
	switch (which) {
	case ITIMER_REAL:
		timer = &cur->signals.itimer_real;
		break;
	case ITIMER_VIRTUAL:
		timer = &cur->signals.itimer_virtual;
		break;
	case ITIMER_PROF:
		timer = &cur->signals.itimer_prof;
		break;
	default:
		return -EINVAL;
	}

	// Copy old value if requested
	if (old_value_ptr) {
		if (copy_to_user((void *)old_value_ptr, timer,
				 sizeof(struct k_itimerval)) != 0) {
			return -EFAULT;
		}
	}

	// Set new value if provided
	if (new_value_ptr) {
		if (copy_from_user(timer, (void *)new_value_ptr,
				   sizeof(struct k_itimerval)) != 0) {
			return -EFAULT;
		}
	}

	return 0;
}

// SYS_GETITIMER - get interval timer
int64_t sys_getitimer(uint64_t which, uint64_t curr_value_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_itimerval *timer;
	switch (which) {
	case ITIMER_REAL:
		timer = &cur->signals.itimer_real;
		break;
	case ITIMER_VIRTUAL:
		timer = &cur->signals.itimer_virtual;
		break;
	case ITIMER_PROF:
		timer = &cur->signals.itimer_prof;
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user((void *)curr_value_ptr, timer,
			 sizeof(struct k_itimerval)) != 0) {
		return -EFAULT;
	}

	return 0;
}

// SYS_TIMER_CREATE - create POSIX timer
int64_t sys_timer_create(uint64_t clockid, uint64_t sevp_ptr,
			 uint64_t timerid_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_sigevent sevp;
	if (sevp_ptr) {
		if (copy_from_user(&sevp, (void *)sevp_ptr,
				   sizeof(struct k_sigevent)) != 0) {
			return -EFAULT;
		}
	} else {
		// Default
		mm_memset(&sevp, 0, sizeof(sevp));
		sevp.sigev_notify = SIGEV_SIGNAL;
		sevp.sigev_signo = SIGALRM;
	}

	ktimer_t tid = timer_create_internal(cur, (clockid_t)clockid, &sevp);
	if (tid < 0) {
		return -EAGAIN;
	}

	if (copy_to_user((void *)timerid_ptr, &tid, sizeof(ktimer_t)) != 0) {
		timer_delete_internal(tid);
		return -EFAULT;
	}

	return 0;
}

// SYS_TIMER_SETTIME - set POSIX timer
int64_t sys_timer_settime(uint64_t timerid, uint64_t flags,
			  uint64_t new_value_ptr, uint64_t old_value_ptr)
{
	struct k_itimerspec new_value, old_value;

	if (copy_from_user(&new_value, (void *)new_value_ptr,
			   sizeof(struct k_itimerspec)) != 0) {
		return -EFAULT;
	}

	int ret = timer_settime_internal((ktimer_t)timerid, (int)flags,
					 &new_value,
					 old_value_ptr ? &old_value : NULL);
	if (ret < 0) {
		return ret;
	}

	if (old_value_ptr) {
		if (copy_to_user((void *)old_value_ptr, &old_value,
				 sizeof(struct k_itimerspec)) != 0) {
			return -EFAULT;
		}
	}

	return 0;
}

// SYS_TIMER_GETTIME - get POSIX timer
int64_t sys_timer_gettime(uint64_t timerid, uint64_t curr_value_ptr)
{
	struct k_itimerspec curr_value;

	int ret = timer_gettime_internal((ktimer_t)timerid, &curr_value);
	if (ret < 0) {
		return ret;
	}

	if (copy_to_user((void *)curr_value_ptr, &curr_value,
			 sizeof(struct k_itimerspec)) != 0) {
		return -EFAULT;
	}

	return 0;
}

// SYS_TIMER_GETOVERRUN - get timer overrun count
int64_t sys_timer_getoverrun(uint64_t timerid)
{
	return timer_getoverrun_internal((ktimer_t)timerid);
}

// SYS_TIMER_DELETE - delete POSIX timer
int64_t sys_timer_delete(uint64_t timerid)
{
	return timer_delete_internal((ktimer_t)timerid);
}

/* Sleep on the high-resolution timer queue until an absolute monotonic
 * deadline; on a signal, write what was left to *rem_ptr (relative) and
 * report EINTR.  Shared by nanosleep and clock_nanosleep. */
static int64_t sleep_until_ns(uint64_t abs_ns, uint64_t rem_ptr)
{
	uint64_t remaining = 0;
	int rc = hrtimer_sleep_until(abs_ns, &remaining);

	if (rc == -EINTR && rem_ptr) {
		struct k_timespec rem;

		rem.tv_sec = remaining / 1000000000ULL;
		rem.tv_nsec = remaining % 1000000000ULL;
		if (copy_to_user((void *)rem_ptr, &rem, sizeof(rem)) != 0)
			return -EFAULT;
	}
	return rc;
}

static int timespec_valid(const struct k_timespec *ts)
{
	return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000L;
}

// SYS_NANOSLEEP - sleep with nanosecond precision
int64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr)
{
	struct k_timespec req;

	if (copy_from_user(&req, (void *)req_ptr, sizeof(struct k_timespec)) !=
	    0)
		return -EFAULT;
	if (!timespec_valid(&req))
		return -EINVAL;

	uint64_t total_ns =
		(uint64_t)req.tv_sec * 1000000000ULL + (uint64_t)req.tv_nsec;
	if (total_ns == 0)
		return 0;
	return sleep_until_ns(hrtimer_now_ns() + total_ns, rem_ptr);
}

/* SYS_CLOCK_NANOSLEEP - nanosleep against a named clock, optionally to an
 * absolute deadline (TIMER_ABSTIME).  An absolute sleep reports no
 * remaining time, as the deadline itself is the remaining time. */
int64_t sys_clock_nanosleep(uint64_t clk_id, uint64_t flags, uint64_t req_ptr,
			    uint64_t rem_ptr)
{
	struct k_timespec req;

	switch (clk_id) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_BOOTTIME:
	case CLOCK_REALTIME_COARSE:
	case CLOCK_MONOTONIC_COARSE:
		break;
	case CLOCK_PROCESS_CPUTIME:
	case CLOCK_THREAD_CPUTIME:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
	if (copy_from_user(&req, (void *)req_ptr, sizeof(struct k_timespec)) !=
	    0)
		return -EFAULT;
	if (!timespec_valid(&req))
		return -EINVAL;

	uint64_t req_ns =
		(uint64_t)req.tv_sec * 1000000000ULL + (uint64_t)req.tv_nsec;
	uint64_t abs_ns;

	if (flags & TIMER_ABSTIME) {
		/* Translate a REALTIME deadline into the monotonic clock the
		 * timers run on; the two differ by the boot epoch. */
		if (clk_id == CLOCK_REALTIME || clk_id == CLOCK_REALTIME_COARSE) {
			uint64_t epoch_ns =
				timer_get_boot_epoch() * 1000000000ULL;
			abs_ns = req_ns > epoch_ns ? req_ns - epoch_ns : 0;
		} else {
			abs_ns = req_ns;
		}
		return sleep_until_ns(abs_ns, 0);
	}
	if (req_ns == 0)
		return 0;
	return sleep_until_ns(hrtimer_now_ns() + req_ns, rem_ptr);
}

// SYS_CLOCK_GETTIME - get time from specified clock
int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_ptr)
{
	if (!validate_user_ptr(tp_ptr, sizeof(struct k_timespec))) {
		return -EFAULT;
	}

	struct k_timespec tp;
	uint64_t total_us = timer_get_precise_us();

	uint64_t total_secs = total_us / 1000000ULL;
	uint64_t frac_ns = (total_us % 1000000ULL) * 1000ULL;

	switch (clk_id) {
	case CLOCK_REALTIME:
	case CLOCK_REALTIME_COARSE:
		tp.tv_sec = timer_get_boot_epoch() + total_secs;
		tp.tv_nsec = frac_ns;
		break;
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_MONOTONIC_COARSE:
	case CLOCK_BOOTTIME:
		/* One clock behind all four names: nothing here slews or
		 * suspends, so RAW and BOOTTIME cannot differ from MONOTONIC,
		 * and the COARSE variants are simply not any coarser. */
		tp.tv_sec = total_secs;
		tp.tv_nsec = frac_ns;
		break;
	default:
		/* Per-thread CPU clocks for OTHER threads:
		 * pthread_getcpuclockid() encodes the target's tid as
		 * (CLOCK_TID_CPUTIME_BASE | tid).  The same tick accounting
		 * that answers CLOCK_THREAD_CPUTIME_ID for the caller answers
		 * it for any live thread; a dead or unknown tid is EINVAL,
		 * which is what POSIX says a dangling clockid earns. */
		if (clk_id >= 0x40000000) {
			uint32_t tid = (uint32_t)(clk_id & 0x3FFFFFFF);
			task_t *t = sched_find_task_by_id(tid);
			if (!t)
				return -EINVAL;
			uint64_t ticks = t->utime_ticks + t->stime_ticks;
			uint64_t hz = timer_get_frequency();
			if (hz == 0)
				hz = 100;
			tp.tv_sec = ticks / hz;
			tp.tv_nsec = (long)((ticks % hz) *
					    (1000000000ULL / hz));
			break;
		}
		return -EINVAL;
	case 2: // CLOCK_PROCESS_CPUTIME_ID
	case 3: // CLOCK_THREAD_CPUTIME_ID
		// CPU time consumed, not time elapsed.  These used to return
		// uptime, which is the same number only for a process that
		// never sleeps -- so anything measuring its own cost (clock(),
		// a profiler, a benchmark) was reading wall time and calling it
		// CPU time.  The tick handler already charges user and system
		// ticks per task, so report those.
		//
		// PROCESS and THREAD are the same figure here because the
		// accounting is per task and a thread IS a task; for a
		// single-threaded process the two agree exactly.
		{
			task_t *cur = sched_current();
			uint64_t ticks =
				cur ? cur->utime_ticks + cur->stime_ticks : 0;
			uint32_t freq = timer_get_frequency();
			if (freq == 0)
				freq = 100;
			tp.tv_sec = (int64_t)(ticks / freq);
			tp.tv_nsec = (int64_t)((ticks % freq) *
					       (1000000000ULL / freq));
		}
		break;
	}

	if (copy_to_user((void *)tp_ptr, &tp, sizeof(tp)) != 0) {
		return -EFAULT;
	}

	return 0;
}

// SYS_CLOCK_GETRES - get clock resolution
int64_t sys_clock_getres(uint64_t clk_id, uint64_t res_ptr)
{
	if (clk_id > CLOCK_BOOTTIME && clk_id < 0x40000000) {
		return -EINVAL;
	}

	if (res_ptr) {
		if (!validate_user_ptr(res_ptr, sizeof(struct k_timespec))) {
			return -EFAULT;
		}

		struct k_timespec res;
		// Resolution = 1 tick in nanoseconds
		res.tv_sec = 0;
		res.tv_nsec = 1000000000 / timer_get_frequency();

		if (copy_to_user((void *)res_ptr, &res, sizeof(res)) != 0) {
			return -EFAULT;
		}
	}

	return 0;
}
