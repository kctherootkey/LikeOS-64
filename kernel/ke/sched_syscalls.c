// LikeOS-64 -- the scheduling syscalls.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/smp.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>

// SYS_YIELD - yield CPU to other runnable tasks
// Moves current task to back of run queue and immediately reschedules.
// Returns 0 on success. In a preemptive kernel this is a hint to the
// scheduler that the caller is willing to give up its remaining timeslice.
int64_t sys_yield(void)
{
	task_t *cur = sched_current();
	if (!cur) {
		return 0;
	}

	// Reset time slice - we're voluntarily giving it up
	cur->remaining_ticks = 0;
	cur->state = TASK_READY;

	// Immediate reschedule
	sched_schedule();

	return 0;
}

// Scheduling policies
#define SCHED_NORMAL 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5
#define SCHED_DEADLINE 6

// CPU set for affinity
#define CPU_SETSIZE 64
typedef struct {
	uint64_t bits[CPU_SETSIZE / 64];
} cpu_set_t;

// SYS_SCHED_SETAFFINITY - bind thread to specific CPUs
int64_t sys_sched_setaffinity(uint64_t pid, uint64_t cpusetsize,
			      uint64_t mask_ptr)
{
	(void)cpusetsize;

	if (!validate_user_ptr(mask_ptr, sizeof(uint64_t))) {
		return -EFAULT;
	}

	/* Read the mask BEFORE taking the task-list lock.  Touching user memory
	 * under a spinlock with interrupts off can fault on a demand-paged page
	 * and then wait, with interrupts disabled, for a completion that cannot
	 * be delivered on this CPU. */
	smap_disable();
	uint64_t mask = *(uint64_t *)mask_ptr;
	smap_enable();

	// Validate: at least one CPU must be set
	if (mask == 0) {
		return -EINVAL;
	}

	/* Resolve, check and apply under the task-list lock.  sched_find_task_by_id
	 * returns a bare pointer with the lock already dropped, so a target
	 * resolved that way can be reaped before it is used -- and the
	 * permission check below reads the target's credentials, which is
	 * exactly the dereference that must not race a free. */
	uint64_t flags;
	spin_lock_irqsave(&g_task_list_lock, &flags);

	task_t *target = (pid == 0) ? sched_current() :
				      sched_find_task_by_id_locked((uint32_t)pid);

	if (!target) {
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return -ESRCH;
	}

	/* Pinning a process to one CPU is a way to interfere with it, so it
	 * takes the same right as sending it a signal: the caller must own it,
	 * or be privileged.  This was previously unchecked, which let any user
	 * confine any other user's processes -- root's included -- to a single
	 * CPU.  The kill(2) rule rather than the stricter read rule is the
	 * right one here: this perturbs the process, it does not read anything
	 * private out of it.  Signal 0 is the probe form -- permission is the
	 * question, nothing is delivered. */
	int64_t perm = signal_target_check(target, 0);
	if (perm != 0) {
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return perm;
	}

	// Store the full affinity mask (0 means all CPUs allowed)
	target->cpu_affinity = mask;

	/* Migration to an allowed CPU.
	 *
	 * NEVER rewrite target->on_cpu here: on_cpu names the run queue the
	 * task is (or will be) linked on, and the scheduler's re-enqueue paths
	 * use this_cpu while rq_remove uses on_cpu.  Flipping on_cpu on a
	 * RUNNING or queued task desynchronises the two, and a later rq_remove
	 * then operates on the wrong queue — which used to truncate the real
	 * queue and permanently strand every task linked behind (unkillable
	 * READY tasks; showed up as rare teststress hangs).
	 *
	 * Also never wake-and-retarget a BLOCKED task onto another CPU from
	 * here: a task can be BLOCKED but still executing on its old CPU (the
	 * window between state=TASK_BLOCKED and its context switch completing).
	 * Every waker in the kernel re-enqueues to the task's OWN on_cpu so
	 * that window resolves locally; handing the task to a different CPU in
	 * that window lets two CPUs act on one context (observed as an idle
	 * task IRET-ing into user code + shifted-frame kernel stack smashes).
	 *
	 * So: only record the mask and request a reschedule.  The load
	 * balancer migrates the task lazily with both run-queue locks held,
	 * honouring cpu_affinity — the one protocol that is safe.
	 */
	if (!(mask & (1ULL << target->on_cpu)))
		target->need_resched = 1;

	spin_unlock_irqrestore(&g_task_list_lock, flags);
	return 0;
}

// SYS_SCHED_GETAFFINITY - get CPU affinity mask
int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
			      uint64_t mask_ptr)
{
	(void)cpusetsize;

	if (!validate_user_ptr(mask_ptr, sizeof(uint64_t))) {
		return -EFAULT;
	}

	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	// Return stored affinity mask, or all CPUs if not set
	uint64_t mask = target->cpu_affinity;
	if (mask == 0) {
		// Affinity not set = all CPUs allowed, return mask with all online CPUs
		uint32_t cpu_count = smp_get_cpu_count();
		mask = (1ULL << cpu_count) - 1;
		if (mask == 0)
			mask = 1; // At least CPU 0
	}

	smap_disable();
	*(uint64_t *)mask_ptr = mask;
	smap_enable();

	return sizeof(uint64_t);
}

// Scheduling parameters
struct sched_param {
	int sched_priority;
};

// SYS_SCHED_SETSCHEDULER - set scheduling policy
int64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy,
			       uint64_t param_ptr)
{
	(void)param_ptr;

	/* Changing another process's scheduling takes the same right as
	 * signalling it -- see sys_sched_setaffinity.  The policy change itself
	 * is still a no-op below, but the gate belongs here now rather than
	 * whenever it grows teeth: an unchecked pid argument is a hole waiting
	 * for the implementation to arrive. */
	uint64_t flags;
	spin_lock_irqsave(&g_task_list_lock, &flags);

	task_t *target = (pid == 0) ? sched_current() :
				      sched_find_task_by_id_locked((uint32_t)pid);

	if (!target) {
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return -ESRCH;
	}

	int64_t perm = signal_target_check(target, 0);
	spin_unlock_irqrestore(&g_task_list_lock, flags);
	if (perm != 0)
		return perm;

	// We only support SCHED_NORMAL for now
	if (policy != SCHED_NORMAL && policy != SCHED_RR) {
		return -EINVAL;
	}

	return 0;
}

// SYS_SCHED_GETSCHEDULER - get scheduling policy
int64_t sys_sched_getscheduler(uint64_t pid)
{
	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	return SCHED_NORMAL; // We use round-robin by default
}

// SYS_SCHED_SETPARAM - set scheduling parameters
int64_t sys_sched_setparam(uint64_t pid, uint64_t param_ptr)
{
	(void)param_ptr;

	/* Same gate as sys_sched_setscheduler, for the same reason. */
	uint64_t flags;
	spin_lock_irqsave(&g_task_list_lock, &flags);

	task_t *target = (pid == 0) ? sched_current() :
				      sched_find_task_by_id_locked((uint32_t)pid);

	if (!target) {
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return -ESRCH;
	}

	int64_t perm = signal_target_check(target, 0);
	spin_unlock_irqrestore(&g_task_list_lock, flags);
	if (perm != 0)
		return perm;

	// Accept but ignore (we use fixed round-robin)
	return 0;
}

// SYS_SCHED_GETPARAM - get scheduling parameters
int64_t sys_sched_getparam(uint64_t pid, uint64_t param_ptr)
{
	if (!validate_user_ptr(param_ptr, sizeof(struct sched_param))) {
		return -EFAULT;
	}

	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	struct sched_param param = { .sched_priority = 0 };

	smap_disable();
	*(struct sched_param *)param_ptr = param;
	smap_enable();

	return 0;
}

// SYS_SCHED_GET_PRIORITY_MAX - get max priority for policy
int64_t sys_sched_get_priority_max(uint64_t policy)
{
	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		return 99;
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_IDLE:
		return 0;
	default:
		return -EINVAL;
	}
}

// SYS_SCHED_GET_PRIORITY_MIN - get min priority for policy
int64_t sys_sched_get_priority_min(uint64_t policy)
{
	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		return 1;
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_IDLE:
		return 0;
	default:
		return -EINVAL;
	}
}

// SYS_SCHED_RR_GET_INTERVAL - get round-robin time quantum
int64_t sys_sched_rr_get_interval(uint64_t pid, uint64_t tp_ptr)
{
	if (!validate_user_ptr(tp_ptr, sizeof(struct k_timespec))) {
		return -EFAULT;
	}

	task_t *target;
	if (pid == 0) {
		target = sched_current();
	} else {
		target = sched_find_task_by_id((uint32_t)pid);
	}

	if (!target) {
		return -ESRCH;
	}

	// Return time slice (at 100Hz, 2 ticks = 20ms)
	struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 }; // 20ms

	smap_disable();
	*(struct k_timespec *)tp_ptr = ts;
	smap_enable();

	return 0;
}
