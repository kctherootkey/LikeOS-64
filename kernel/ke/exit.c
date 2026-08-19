// LikeOS-64 -- exit, exit_group and waitpid.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>
#include <kernel/uapi/rusage.h>

// SYS_EXIT - exit task
__attribute__((noreturn)) void sys_exit(uint64_t status)
{
	task_t *cur = sched_current();

	/* Stop for the tracer BEFORE dying, if it asked.
	 *
	 * A thread is the one thing a debugger cannot notice on its own.  Its
	 * creation is announced (PTRACE_EVENT_CLONE); its death was not, and
	 * nothing else reports it either -- a thread is not its tracer's child,
	 * so no waitpid ever names it, and the tracer's thread list only
	 * refreshes when the process next stops.  A program whose threads come
	 * and go therefore showed a list of threads that had been alive at some
	 * point, with the dead ones only disappearing at the next breakpoint or
	 * ^C.
	 *
	 * The stop is here, at the top of _exit, because this is the last point
	 * at which the thread still exists: registers readable, memory mapped,
	 * id valid.  Its exit status travels as the event's value, which is
	 * what PTRACE_GETEVENTMSG returns.  The tracer resumes it with
	 * PTRACE_CONT and it dies below.
	 *
	 * Opt-in like every other event, and this one matters: a tracer that
	 * does not ask must never be stopped for, or a thread would park here
	 * with nobody to release it.  A tracer that dies while it is parked
	 * detaches it, which resumes it (see task_ptrace_detach). */
	if (cur && cur->tracer_pid != 0 &&
	    (cur->ptrace_options & PTRACE_O_TRACEEXIT)) {
		/* Set BEFORE the stop, not after.  The flag is what stops
		 * anything else being reported for this task, and the window it
		 * has to cover starts the moment the tracer can see the event
		 * -- the tracer resumes us from inside task_ptrace_stop, and
		 * from then on we are a thread it has already dropped. */
		cur->ptrace_exiting = 1;
		task_ptrace_stop(cur, SIGTRAP, PTRACE_EVENT_EXIT,
				 (unsigned long)(status & 0xFF));
	}

	if (cur) {
		sched_mark_task_exited(cur, (int)status);
	}
	/* Switch away for good.  sched_schedule() normally never returns for an
     * exited task — it switches to another task (or idle) and this frame is
     * abandoned.  But it CAN return in rare scheduler edge cases (e.g. the
     * idle task was momentarily unselectable on this CPU, so the zombie is
     * left as current_task).  In that case we must NOT park in `cli; hlt`:
     * a CPU halted with interrupts disabled can no longer ack TLB-shootdown
     * IPIs, which permanently wedges every other CPU spinning in
     * smp_tlb_shootdown_sync() — observed as endless
     * "TLB shootdown sync timeout (gen=...)" with this CPU's gen frozen and
     * a diagnostic NMI sampling it stuck at this very loop.
     *
     * Keep interrupts ENABLED and retry: with IRQs on, the TLB-shootdown IPI
     * is serviced (unblocking the rest of the system) and the timer's
     * sched_preempt() evacuates this zombie to the idle task via its own
     * idle fallback, so we are switched off this CPU at the next tick. */
	sched_exit_park();
}

// SYS_WAIT4/SYS_WAITPID - wait for child process
// In a preemptive kernel, this BLOCKS until a child exits (unless WNOHANG)
int64_t sys_waitpid(int64_t pid, uint64_t status_ptr, uint64_t options,
		    uint64_t rusage_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	// Check if there are any children at all
	/* Children hang off the thread-group leader: they belong to the process,
	 * so any thread of it may wait for them.  Only the child list is read
	 * through the leader -- everything else here, above all the blocking,
	 * belongs to the thread that actually called. */
	task_t *owner = cur->group_leader ? cur->group_leader : cur;

	/* A tracer waits for its tracees as well as for its children, and a
	 * debugger's tracees are usually neither: it attaches to processes it
	 * did not create.  So "no children" is only ECHILD if there are no
	 * tracees either. */
	if (!owner->first_child && !task_has_tracees(owner, pid))
		return -ECHILD;

	// Loop until we find a reportable child or get interrupted
	while (1) {
		/* Sampled BEFORE the scans below, so that any trace stop
		 * recorded from here on is either found by them or shows up as
		 * a changed counter at the point where sleeping is decided.
		 * Sampling it later would leave the gap it exists to close. */
		uint64_t trace_seq = owner->ptrace_notify_seq;

		task_t *child = NULL; /* zombie to reap */
		task_t *stopped = NULL; /* WUNTRACED: freshly stopped child */
		task_t *continued = NULL; /* WCONTINUED: freshly continued */
		int matched_any = 0;

		/* One scan of the child list handles every pid form:
		 *   pid > 0   that child only
		 *   pid == -1 any child
		 *   pid == 0  any child in the caller's process group
		 *   pid < -1  any child in process group -pid */
		/* Under g_wait_lock: the list is edited from other CPUs (a
		 * child exiting, a dying ancestor reparenting a whole list into
		 * ours) and walking it unlocked both reads half-spliced links
		 * and can miss the very child we are about to sleep for.  Only
		 * the walk is inside -- the copy_to_user()s below must not run
		 * with a spinlock held. */
		uint64_t scan_flags;

		spin_lock_irqsave(&g_wait_lock, &scan_flags);
		for (task_t *c = owner->first_child; c; c = c->next_sibling) {
			if (pid > 0 && c->id != (uint32_t)pid)
				continue;
			if (pid == 0 && c->pgid != cur->pgid)
				continue;
			if (pid < -1 && c->pgid != (int)(-pid))
				continue;
			matched_any = 1;
			if (c->has_exited) {
				child = c;
				break;
			}
			if ((options & 2 /* WUNTRACED */) && c->jc_stop_signo &&
			    !stopped)
				stopped = c;
			if ((options & 8 /* WCONTINUED */) && c->jc_continued &&
			    !continued)
				continued = c;
		}
		spin_unlock_irqrestore(&g_wait_lock, scan_flags);

		/* Trace stops, before the ECHILD decision: a tracer's tracees
		 * are usually not its children, so `matched_any' says nothing
		 * about them.  Reported ahead of a stopped CHILD because a
		 * traced child produces both kinds of record and the tracer is
		 * the one that has to see the trace stop -- consuming it as a
		 * job-control stop would leave the tracee parked with its
		 * tracer still waiting.
		 *
		 * Taken outside g_wait_lock: this takes the task-list lock, and
		 * nesting the two would invent a lock order nothing else here
		 * uses. */
		{
			int tstatus = 0;
			int tpid = task_reap_trace_stop(owner, pid, &tstatus);

			if (tpid) {
				if (status_ptr &&
				    validate_user_ptr(status_ptr, sizeof(int)))
					copy_to_user((void *)status_ptr,
						     &tstatus, sizeof(tstatus));
				return tpid;
			}
		}

		/* Does this caller have TRACEES to wait for?  Sampled here,
		 * outside g_wait_lock, because task_has_tracees() takes the
		 * task-list lock and the two are never nested -- and it is
		 * needed twice: once for the ECHILD decision and once when
		 * deciding to sleep.  Only asked when the child scan found
		 * nothing, which is also the only case where it can change
		 * either answer (a caller with a matching child has a child
		 * list, so it is going to wait either way). */
		bool has_tracees = matched_any ? false :
						 task_has_tracees(owner, pid) != 0;

		if (!matched_any && !has_tracees)
			return -ECHILD;

		/* Job-control events: report without reaping. */
		if (!child && stopped) {
			int signo = stopped->jc_stop_signo;
			stopped->jc_stop_signo = 0;
			int status = ((signo & 0xFF) << 8) | 0x7F;
			if (status_ptr &&
			    validate_user_ptr(status_ptr, sizeof(int)))
				copy_to_user((void *)status_ptr, &status,
					     sizeof(status));
			return stopped->id;
		}
		if (!child && continued) {
			continued->jc_continued = 0;
			int status = 0xFFFF; /* conventional "continued" code */
			if (status_ptr &&
			    validate_user_ptr(status_ptr, sizeof(int)))
				copy_to_user((void *)status_ptr, &status,
					     sizeof(status));
			return continued->id;
		}

		if (child) {
			// Found a zombie child - reap it
			int status = 0;
			if (child->term_sig != 0) {
				// Killed by a signal: WIFSIGNALED, low 7 bits =
				// signal number.  Distinguished from exit(128+N)
				// by the explicit term_sig flag — exit_code alone
				// is ambiguous (a program exiting with a status
				// >= 128, e.g. ssh's 255, is NOT a signal death).
				status = child->term_sig & 0x7F;
			} else {
				// Normal exit: exit_code << 8 (WIFEXITED)
				status = (child->exit_code & 0xFF) << 8;
			}

			if (status_ptr &&
			    validate_user_ptr(status_ptr, sizeof(int))) {
				copy_to_user((void *)status_ptr, &status,
					     sizeof(status));
			}

			/* Fill in resource usage from the child's accounting data */
			/* The same struct sys_getrusage reports, and the same
			 * size: this wrote a 56-byte local of its own, which
			 * left the caller's last two fields holding whatever
			 * was in that memory beforehand.  Reporting stale
			 * stack as a context-switch count is its own small
			 * bug; sharing one definition is what stops the two
			 * paths drifting again. */
			if (rusage_ptr &&
			    validate_user_ptr(rusage_ptr,
					      sizeof(struct k_rusage_compat))) {
				struct k_rusage_compat ru;
				for (size_t i = 0; i < sizeof(ru); i++)
					((uint8_t *)&ru)[i] = 0;
				ticks_to_timeval(child->utime_ticks,
						 &ru.ru_utime_sec,
						 &ru.ru_utime_usec);
				ticks_to_timeval(child->stime_ticks,
						 &ru.ru_stime_sec,
						 &ru.ru_stime_usec);
				copy_to_user((void *)rusage_ptr, &ru,
					     sizeof(ru));
			}

			int child_pid = child->id;
			/* Heavy destruction (address space, kernel stack, TLB
			 * shootdown, still-running spin) is deferred to the
			 * dead-thread reaper; running it synchronously here
			 * cost 7-9 ms per reaped child. */
			sched_defer_reap(child);
			return child_pid;
		}

		// No zombie child yet
		if (options & 1) { // WNOHANG
			return 0; // No child exited yet, return immediately
		}

		// Check for pending signal BEFORE blocking - avoids race condition
		if (signal_pending(cur)) {
			return -EINTR;
		}

		/* Block until a child exits or we get a signal.
		 *
		 * The re-check and the transition to BLOCKED happen under
		 * g_wait_lock, which is the same lock a child takes to look at
		 * whether its parent is asleep here.  That is what makes the
		 * decision safe: if a child exits during this window it either
		 * gets the lock first — and we see has_exited below and never
		 * sleep — or it gets it after us, and finds us already BLOCKED
		 * with wait_channel set, so it wakes us.
		 *
		 * Interrupts-off alone (what this used to do) excludes a timer
		 * on THIS cpu and nothing at all on the others, so on SMP the
		 * wake could fall in the gap.  Nothing recovers from that: a
		 * waitpid sleeper arms no timeout, and SIGCHLD's default action
		 * is ignore so it is never left pending — the periodic sweep
		 * that re-wakes blocked tasks has no reason to touch it, and it
		 * sleeps for good.
		 *
		 * Also caught here: a child that vanished entirely (a parent
		 * ignoring SIGCHLD has its children reaped as they exit).  The
		 * list can go empty while we are in this loop, and the scan at
		 * the top of it turns that into the ECHILD it should be. */
		uint64_t irq_flags;
		spin_lock_irqsave(&g_wait_lock, &irq_flags);

		bool found_zombie = false;
		task_t *zombie_check = owner->first_child;
		while (zombie_check) {
			if (zombie_check->has_exited ||
			    ((options & 2) && zombie_check->jc_stop_signo) ||
			    ((options & 8) && zombie_check->jc_continued)) {
				found_zombie = true;
				break;
			}
			zombie_check = zombie_check->next_sibling;
		}

		if (found_zombie || (!owner->first_child && !has_tracees)) {
			/* Something to report, or nothing left to wait for:
			 * either way the top of the loop decides, not us.
			 *
			 * `has_tracees' is what keeps a TRACER out of this arm.
			 * A debugger that attached to a process it did not
			 * create has no children at all, so "no child list"
			 * described it perfectly and sent it back round the
			 * loop -- for ever, at full speed, taking the task-list
			 * lock with interrupts off twice per turn.  That is not
			 * a slow debugger, it is a CPU pinned in the kernel
			 * starving every other CPU that wants the same lock.
			 * It has something to wait for; it belongs asleep. */
			spin_unlock_irqrestore(&g_wait_lock, irq_flags);
			continue;
		}

		/* Same re-check for tracees, via the notification counter.
		 *
		 * A trace stop recorded between the scan above and this point
		 * would otherwise be slept through: task_ptrace_stop() records
		 * the event and bumps the counter before waking the tracer, so
		 * a tracer that has not yet marked itself BLOCKED gets a wake
		 * that lands on a task which was not asleep, and nothing
		 * re-sends it.  Comparing the counter catches exactly that
		 * window -- and it is a single field read, so it does not need
		 * the task-list lock inside this one. */
		if (owner->ptrace_notify_seq != trace_seq) {
			spin_unlock_irqrestore(&g_wait_lock, irq_flags);
			continue;
		}

		/* wait_channel before state: a reader that sees BLOCKED must
		 * also see the channel it is blocked on.  (Both stores are
		 * under the lock, so this only matters for the lock-free
		 * glances other wakers take.) */
		cur->wait_channel = cur; // Waiting for our own children
		cur->state = TASK_BLOCKED;
		spin_unlock_irqrestore(&g_wait_lock, irq_flags);

		sched_schedule();
		// NOTE: Do NOT set cur->state = TASK_READY here!
		// When sched_schedule() returns, the scheduler has already set us
		// to TASK_RUNNING.  Overwriting with TASK_READY causes a race on SMP
		// where sched_wake_expired_sleepers sees READY + !on_rq and enqueues
		// us on another CPU while we're still running → double scheduling.
		cur->wait_channel = 0;

		// Check if we were woken by a signal
		if (signal_pending(cur)) {
			return -EINTR;
		}

		// Check if we were terminated
		if (cur->has_exited) {
			return -EINTR;
		}

		// Loop back to check for zombie children again
	}
}

// SYS_EXIT_GROUP - terminate all threads in the process
void sys_exit_group(uint64_t status)
{
	task_t *cur = sched_current();
	if (!cur) {
		while (1) {
			__asm__ volatile("hlt");
		}
	}

	task_t *leader = cur->group_leader;
	if (!leader)
		leader = cur;

	/* Same check, and for the same reason, as sched_kill_thread_group():
	 * group_leader is a bare pointer that nothing clears in the surviving
	 * threads, and the writes just below are made with no lock held.  A
	 * leader that has already been freed is written into poisoned memory
	 * and then walked from, which faults non-recoverably once the lock is
	 * taken.  Fall back to this thread's own group of one. */
	if (leader != cur && !task_ptr_ok(leader)) {
		WARN_ON_ONCE(1);
		kprintf("BUG: pid %d group_leader is unusable (%p) - exiting this thread only\n",
			cur->id, (void *)leader);
		leader = cur;
	}

	// Mark the group as exiting to prevent new threads
	leader->group_exiting = true;
	leader->group_exit_code = (int)status;

	/*
	 * Signal every other thread in the group to exit.
	 *
	 * COLLECT under g_task_list_lock, SIGNAL after releasing it.
	 *
	 * The list is maintained by thread_group_remove(), which unlinks a
	 * dying thread under that lock -- and the task_t is freed and poisoned
	 * shortly afterwards.  Walking it unlocked therefore races a thread
	 * exiting on another CPU and follows `thread_group_next' straight into
	 * freed memory: observed as a general protection fault in here with
	 * t == 0xdeadbeefdeadbeef, taking the whole system down.  It went
	 * unnoticed for as long as it did because nothing routed an ordinary
	 * process exit through exit_group; now that _exit() does, every exit
	 * of a threaded program runs this loop.
	 *
	 * Signalling cannot happen under the lock -- sched_signal_task() takes
	 * scheduler locks and may queue a siginfo -- hence the two passes.
	 */
	/* The batch has to hold the WHOLE group.
	 *
	 * A fixed array used to cap it, and the threads past the cap were
	 * dropped with a warning -- nothing ever came back for them.  That
	 * leaves live threads running in a process whose parent has already
	 * been told it finished, still holding its descriptors and its
	 * address space.  Size the batch from the group instead, and fall
	 * back to the on-stack array only when it fits, so the common case
	 * allocates nothing on the exit path. */
	task_t *stack_targets[TASK_GROUP_KILL_MAX];
	task_t **targets = stack_targets;
	int capacity = TASK_GROUP_KILL_MAX;
	int ntargets = 0;
	int overflow = 0;
	uint64_t tg_flags;

	if (leader->nr_threads >= TASK_GROUP_KILL_MAX) {
		/* +2: one for the leader, one for a thread that appeared
		 * between reading the count and taking the lock. */
		int want = leader->nr_threads + 2;
		task_t **big = kalloc((size_t)want * sizeof(task_t *));

		if (big) {
			targets = big;
			capacity = want;
		}
	}

	spin_lock_irqsave(&g_task_list_lock, &tg_flags);
	{
		task_t *t = leader;
		int guard = 0;

		do {
			if (t != cur && !t->has_exited) {
				if (ntargets < capacity)
					targets[ntargets++] = t;
				else
					overflow++;
			}
			t = t->thread_group_next;
			/* Same guard as sched_kill_thread_group: a broken ring
			 * followed with this lock held and interrupts off
			 * wedges the processor rather than the process. */
			if (!task_ptr_ok(t) ||
			    ++guard > capacity + TASK_GROUP_KILL_MAX) {
				WARN_ON_ONCE(1);
				kprintf("BUG: thread group ring of tgid %d broken at %p (guard %d)\n",
					leader->id, (void *)t, guard);
				break;
			}
		} while (t != leader);
	}
	spin_unlock_irqrestore(&g_task_list_lock, tg_flags);

	/* Only reachable if the group grew past the count we sized against,
	 * which group_exiting is supposed to prevent. */
	WARN_ON(overflow != 0);

	for (int i = 0; i < ntargets; i++)
		sched_signal_task(targets[i], SIGKILL);

	if (targets != stack_targets)
		kfree(targets);

	// Now exit ourselves
	sched_mark_task_exited(cur, (int)status);

	/* Same hardened park as sys_exit: sched_schedule() can return in the
	 * rare zombie edge case, and a bare `hlt` here (without STI and without
	 * retrying the switch) parks the CPU for a full timer tick per exit —
	 * and with IRQs off would wedge TLB shootdowns (see sys_exit). */
	sched_exit_park();
}
