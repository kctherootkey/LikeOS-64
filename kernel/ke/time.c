// LikeOS-64 -- time of day, clocks, sleeping and interval timers.
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


// SYS_NANOSLEEP - sleep with nanosecond precision
// Uses timer-based wakeup: set wakeup_tick and block, timer IRQ wakes us
int64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_timespec req;
	if (copy_from_user(&req, (void *)req_ptr, sizeof(struct k_timespec)) !=
	    0) {
		return -EFAULT;
	}

	// Calculate ticks to sleep using measured timer frequency.
	//
	// Two boundary corrections vs. the obvious floor division:
	//
	//   1. ROUND UP nsec→ticks.  For sub-tick requests (e.g. usleep(1500)
	//      at 100 Hz) floor gives 0; we'd then clamp to 1 tick which is
	//      fine, but for requests that fall between tick multiples (e.g.
	//      15 ms at 100 Hz, floor = 1) the original code returned after
	//      only 10 ms — less than requested.  Ceiling math fixes that.
	//
	//   2. ADD ONE EXTRA TICK to absorb the partial-tick uncertainty at
	//      the start of the sleep.  timer_ticks() was read at some
	//      unknown fraction ε ∈ [0, 1 tick) past the most recent timer
	//      IRQ; the next timer IRQ fires after (1 tick − ε) wall time,
	//      and subsequent IRQs are 1 tick apart.  Without the +1, a 100
	//      ms request at 100 Hz could return after as little as 9·10 ms
	//      = 90 ms of wall time (when ε ≈ 0).  Combined with TSC vs PIT
	//      calibration drift in clock_gettime, this is why
	//      "Timer accuracy under CPU load" reports 79 ms for a 100 ms
	//      usleep and fails the >= 80 ms check.
	uint32_t freq = timer_get_frequency();
	if (freq == 0)
		freq = 100;
	uint64_t total_ns =
		(uint64_t)req.tv_sec * 1000000000ULL + (uint64_t)req.tv_nsec;
	uint64_t ticks =
		(total_ns * (uint64_t)freq + 999999999ULL) / 1000000000ULL;
	if (ticks == 0 && total_ns > 0) {
		ticks = 1;
	}
	if (ticks > 0) {
		ticks +=
			1; // Partial-tick boundary compensation (see comment above).
	}

	uint64_t start = timer_ticks();
	uint64_t end = start + ticks;

	// Loop until sleep time expires or interrupted by signal
	while (timer_ticks() < end) {
		// Check for pending signal BEFORE blocking
		if (signal_pending(cur)) {
			if (rem_ptr) {
				uint64_t now = timer_ticks();
				uint64_t elapsed = now - start;
				uint64_t remaining =
					(elapsed < ticks) ? ticks - elapsed : 0;
				struct k_timespec rem;
				rem.tv_sec = remaining / freq;
				rem.tv_nsec = (uint64_t)(remaining % freq) *
					      1000000000ULL / freq;
				copy_to_user((void *)rem_ptr, &rem,
					     sizeof(struct k_timespec));
			}
			cur->wakeup_tick = 0;
			return -EINTR;
		}

		// Set wakeup timer and block - timer IRQ will wake us
		cur->wakeup_tick = end;
		cur->state = TASK_BLOCKED;
		sched_schedule();

		// Woken up - either by timer expiry or signal
		// Loop will check timer and signal conditions
	}

	cur->wakeup_tick = 0;
	return 0;
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
	case 0: // CLOCK_REALTIME
		tp.tv_sec = timer_get_boot_epoch() + total_secs;
		tp.tv_nsec = frac_ns;
		break;
	case 1: // CLOCK_MONOTONIC
		tp.tv_sec = total_secs;
		tp.tv_nsec = frac_ns;
		break;
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
	default:
		return -EINVAL;
	}

	if (copy_to_user((void *)tp_ptr, &tp, sizeof(tp)) != 0) {
		return -EFAULT;
	}

	return 0;
}


// SYS_CLOCK_GETRES - get clock resolution
int64_t sys_clock_getres(uint64_t clk_id, uint64_t res_ptr)
{
	if (clk_id > 3) {
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

