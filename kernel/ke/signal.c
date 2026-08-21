// LikeOS-64 Kernel Signal Implementation
#include <kernel/ke/signal.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/status.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/percpu.h>
#include <kernel/uapi/bug.h>
#include <kernel/fs/icache.h>
#include <kernel/ke/uaccess.h>

// NOTE: Signal delivery now uses per-CPU storage via percpu_t
// The old global syscall_signal_pending is deprecated.

// Global POSIX timer pool
static kernel_timer_t g_posix_timers[MAX_POSIX_TIMERS];
static ktimer_t g_next_timerid = 1;

// Initialize signal state for a new task
void signal_init_task(task_t *task)
{
	BUG_ON(task == NULL);
	if (!task)
		return;

	task_signal_state_t *sig = &task->signals;

	// Clear all handlers to default
	for (int i = 0; i < NSIG; i++) {
		sig->action[i].sa_handler = SIG_DFL;
		sig->action[i].sa_flags = 0;
		sig->action[i].sa_restorer = NULL;
		sigemptyset_k(&sig->action[i].sa_mask);
	}

	// Clear blocked and pending masks
	sigemptyset_k(&sig->blocked);
	sigemptyset_k(&sig->pending);
	sigemptyset_k(&sig->saved_mask);

	sig->pending_queue = NULL;
	sig->in_sigsuspend = 0;

	// Clear alternate stack
	sig->altstack.ss_sp = NULL;
	sig->altstack.ss_flags = SS_DISABLE;
	sig->altstack.ss_size = 0;

	// Clear timers
	mm_memset(&sig->itimer_real, 0, sizeof(sig->itimer_real));
	mm_memset(&sig->itimer_virtual, 0, sizeof(sig->itimer_virtual));
	mm_memset(&sig->itimer_prof, 0, sizeof(sig->itimer_prof));
	sig->alarm_ticks = 0;

	// Clear signal frame address
	sig->signal_frame_depth = 0;
}

// Reset signal dispositions across execve.
// POSIX: caught signals are reset to SIG_DFL; ignored signals stay ignored;
// the signal mask and the set of pending signals are preserved.  Without this,
// a handler address installed before exec would survive into the new program
// (where that address is meaningless) and crash when the signal is delivered.
void signal_reset_on_exec(task_t *task)
{
	if (!task)
		return;

	task_signal_state_t *sig = &task->signals;

	for (int i = 0; i < NSIG; i++) {
		if (sig->action[i].sa_handler != SIG_IGN)
			sig->action[i].sa_handler = SIG_DFL;
		sig->action[i].sa_flags = 0;
		sig->action[i].sa_restorer = NULL;
		sigemptyset_k(&sig->action[i].sa_mask);
	}

	// The alternate signal stack does not survive exec.
	sig->altstack.ss_sp = NULL;
	sig->altstack.ss_flags = SS_DISABLE;
	sig->altstack.ss_size = 0;
	sig->signal_frame_depth = 0;
}

// Copy signal handlers from parent to child during fork
// POSIX: signal dispositions are inherited across fork
void signal_fork_copy(task_t *child, task_t *parent)
{
	BUG_ON(child == NULL || parent == NULL);
	if (!child || !parent)
		return;

	task_signal_state_t *csig = &child->signals;
	task_signal_state_t *psig = &parent->signals;

	// Copy signal handlers (dispositions are inherited)
	for (int i = 0; i < NSIG; i++) {
		csig->action[i] = psig->action[i];
	}

	// Copy blocked mask (inherited across fork)
	csig->blocked = psig->blocked;

	// Clear pending signals (not inherited - child starts fresh)
	sigemptyset_k(&csig->pending);
	csig->pending_queue = NULL;

	// Clear saved mask and sigsuspend state
	sigemptyset_k(&csig->saved_mask);
	csig->in_sigsuspend = 0;

	// Alternate stack is NOT inherited across fork
	csig->altstack.ss_sp = NULL;
	csig->altstack.ss_flags = SS_DISABLE;
	csig->altstack.ss_size = 0;

	// Timers are NOT inherited (child gets fresh timer state)
	mm_memset(&csig->itimer_real, 0, sizeof(csig->itimer_real));
	mm_memset(&csig->itimer_virtual, 0, sizeof(csig->itimer_virtual));
	mm_memset(&csig->itimer_prof, 0, sizeof(csig->itimer_prof));
	csig->alarm_ticks = 0;

	// Clear signal frame address
	csig->signal_frame_depth = 0;
}

// Cleanup signal state when task exits
void signal_cleanup_task(task_t *task)
{
	BUG_ON(task == NULL);
	if (!task)
		return;

	task_signal_state_t *sig = &task->signals;

	// Free pending signal queue
	pending_signal_t *ps = sig->pending_queue;
	while (ps) {
		pending_signal_t *next = ps->next;
		kfree(ps);
		ps = next;
	}
	sig->pending_queue = NULL;

	// Clear any POSIX timers owned by this task
	for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
		if (g_posix_timers[i].in_use &&
		    g_posix_timers[i].owner_pid == task->id) {
			g_posix_timers[i].in_use = 0;
		}
	}
}

// Allocate a pending signal entry
static pending_signal_t *alloc_pending_signal(void)
{
	pending_signal_t *ps =
		(pending_signal_t *)kalloc(sizeof(pending_signal_t));
	if (ps) {
		mm_memset(ps, 0, sizeof(*ps));
	}
	return ps;
}

/* Drop every PENDING instance of the signals in `mask'.
 *
 * Both halves of the record: the bit that says a signal is pending and the
 * queued description that carries its siginfo.  Clearing one and leaving the
 * other would either resurrect the signal at the next dequeue or strand an
 * allocation nothing will ever collect. */
static void signal_flush_pending_mask(task_signal_state_t *sig,
				      const kernel_sigset_t *mask)
{
	pending_signal_t **pp;

	for (int s = 1; s < NSIG; s++) {
		if (sigismember_k(mask, s))
			sigdelset_k(&sig->pending, s);
	}

	pp = &sig->pending_queue;
	while (*pp) {
		pending_signal_t *ps = *pp;

		if (sigismember_k(mask, ps->sig)) {
			*pp = ps->next;
			kfree(ps);
			continue;
		}
		pp = &ps->next;
	}
}

/* A stop and a continue cancel each other's PENDING instance.
 *
 * The two describe opposite states of the same process, so only the newer one
 * can be true.  An older one still sitting in the queue is not history, it is
 * an instruction that has not run yet -- and it takes effect after the newer
 * one has already been acted on and reported.
 *
 * That is what broke "SIGTSTP stop reported via WUNTRACED".  kill(SIGSTOP)
 * both stops the target and queues the signal, and a stopped task never runs
 * to consume it, so the SIGSTOP stays pending.  kill(SIGCONT) then makes the
 * task runnable with that stale SIGSTOP still queued behind it.  If the parent
 * reaches kill(SIGTSTP) before the child is next scheduled, the child wakes,
 * finds the stale SIGSTOP, takes the default stop action for it, and records
 * jc_stop_signo = SIGSTOP over the SIGTSTP its parent is waiting to hear
 * about -- so waitpid(WUNTRACED) reported a stop for the wrong signal.  How
 * often that happens is purely how late the child is scheduled, which is why
 * it turns from rare into repeatable as the machine gets busier.
 *
 * Per task rather than per thread group: stops and continues are applied to
 * one task here (see sched_signal_task), so that is the scope the pending
 * state has to agree with. */
static void signal_cancel_opposite(task_signal_state_t *sig, int incoming)
{
	kernel_sigset_t flush;

	mm_memset(&flush, 0, sizeof(flush));

	if (incoming == SIGCONT) {
		for (int s = 1; s < NSIG; s++) {
			if (sig_default_action(s) == SIG_DFL_STOP)
				sigaddset_k(&flush, s);
		}
	} else if (sig_default_action(incoming) == SIG_DFL_STOP) {
		sigaddset_k(&flush, SIGCONT);
	} else {
		return;
	}

	signal_flush_pending_mask(sig, &flush);
}

/* Permission check for kill(2): may the calling task signal `target`?  The
 * privileged caller may signal anyone; otherwise the sender's real OR effective
 * uid must equal the target's real OR saved-set uid.  SIGCONT is additionally
 * permitted between processes of the same session.  Mirrors the conventional
 * check so an unprivileged process cannot signal another user's processes. */
int signal_permission(task_t *target, int sig)
{
	if (!target)
		return -EPERM;
	task_t *sender = sched_current();
	if (!sender)
		return 0; /* kernel context: privileged */
	if (capable())
		return 0; /* root may signal anyone */
	uint32_t suid = sender->cred.uid, seuid = sender->cred.euid;
	uint32_t tuid = target->cred.uid, tsuid = target->cred.suid;
	if (suid == tuid || suid == tsuid || seuid == tuid || seuid == tsuid)
		return 0;
	if (sig == SIGCONT && sender->sid == target->sid)
		return 0; /* job-control continue within the session */
	return -EPERM;
}

// Send a signal to a task
int signal_send(task_t *task, int sig, siginfo_t *info)
{
	BUG_ON(task == NULL);
	WARN_ON(sig <= 0 || sig >= NSIG);
	if (!task || sig <= 0 || sig >= NSIG) {
		return -EINVAL;
	}

	/* A task that has already exited takes the signal and discards it.
	 *
	 * Signalling a child that exited but has not been waited for is
	 * ordinary and correct -- there is no way for the sender to know, since
	 * the exit can happen between deciding to signal and doing it, and the
	 * name stays valid until it is reaped.  So this reports success and
	 * does nothing, which is what the caller is entitled to.
	 *
	 * Falling through instead would mark a signal pending on something that
	 * will never run again, and allocate a queued description of it that
	 * only the reap will release.  This used to warn about the situation
	 * rather than handle it, which made a legal race look like a defect. */
	if (task->has_exited || task->state == TASK_ZOMBIE)
		return 0;

	task_signal_state_t *sigstate = &task->signals;
	struct k_sigaction *act = &sigstate->action[sig];

	/* Ahead of the ignore checks below, deliberately.  The disposition
	 * decides whether the ARRIVING signal is delivered; it does not decide
	 * whether the one this supersedes stays queued.  A program that
	 * ignores SIGCONT still has its pending stops cancelled by one, which
	 * is what keeps an ignored continue from being a continue that never
	 * happened. */
	signal_cancel_opposite(sigstate, sig);

	// Check if signal is ignored (except SIGKILL/SIGSTOP)
	if (!sig_kernel_only(sig)) {
		if (act->sa_handler == SIG_IGN) {
			return 0; // Signal ignored
		}
		// Check default action is ignore
		if (act->sa_handler == SIG_DFL &&
		    sig_default_action(sig) == SIG_DFL_IGN) {
			return 0;
		}
	}

	// Add to pending mask
	sigaddset_k(&sigstate->pending, sig);

	/* Keep the description, not just the number.
	 *
	 * SA_SIGINFO says whether the HANDLER is shown the detail; it does not
	 * decide whether the kernel keeps it, and treating it as though it did
	 * threw away the only copy of things nothing can reconstruct later:
	 *
	 *   - a synchronous fault's si_code and si_addr are knowable only at
	 *     the fault.  Discarding them left signal_dequeue synthesising
	 *     si_code = SI_USER and si_addr = 0, so a debugger asking where a
	 *     SIGSEGV faulted was told "sent by kill(), address 0".
	 *   - a traced task's tracer can ask about ANY signal with
	 *     PTRACE_GETSIGINFO, whatever disposition the tracee has.
	 *
	 * Anything else keeps the old behaviour, so an ordinary process pays
	 * no allocation for signals nobody will ask about. */
	int keep_info = info && (act->sa_flags & SA_SIGINFO);

	if (info && !keep_info &&
	    (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
	     sig == SIGFPE || sig == SIGTRAP || task->tracer_pid != 0))
		keep_info = 1;

	if (keep_info) {
		pending_signal_t *ps = alloc_pending_signal();
		if (ps) {
			ps->sig = sig;
			mm_memcpy(&ps->info, info, sizeof(siginfo_t));
			ps->next = sigstate->pending_queue;
			sigstate->pending_queue = ps;
		}
	}

	// Wake BLOCKED tasks when the signal is actionable (not masked).
	// This is needed because some callers (sys_tkill, sys_rt_sigqueueinfo,
	// SIGCHLD delivery) call signal_send() directly without their own wake
	// logic.  Callers that already wake (sched_signal_task, sched_wake_expired_sleepers)
	// are safe: sched_enqueue_ready() guards with !on_rq && TASK_READY,
	// so a double-wake is a harmless no-op.
	if (task->state == TASK_BLOCKED) {
		if (sig_kernel_only(sig) ||
		    !sigismember_k(&sigstate->blocked, sig)) {
			/* Atomic BLOCKED->READY claim: other wakers (futex,
			 * sleep timeout) run under different locks and could
			 * otherwise claim + enqueue the same task twice. */
			if (sched_claim_wake(task, TASK_BLOCKED)) {
				task->wait_channel = NULL;
				task->wakeup_tick = 0;
				sched_enqueue_ready(task);
			}
		}
	}

	return 0;
}

// Send signal to a process group
int signal_send_group(int pgid, int sig, siginfo_t *info)
{
	if (pgid <= 0 || sig <= 0 || sig >= NSIG) {
		return -EINVAL;
	}

	int found = 0;

	// Iterate through all tasks (simplified - would use a proper list in production)
	for (int pid = 1; pid < 256; pid++) {
		task_t *t = sched_find_task_by_id(pid);
		if (t && t->pgid == pgid) {
			signal_send(t, sig, info);
			found++;
		}
	}

	return found > 0 ? 0 : -ESRCH;
}

// Check if any unblocked signals are pending
int signal_pending(task_t *task)
{
	BUG_ON(task == NULL);
	if (!task)
		return 0;

	task_signal_state_t *sig = &task->signals;
	kernel_sigset_t unblocked;

	/* SIGKILL/SIGSTOP are actionable regardless of the mask.  Every path that
	 * installs a mask strips them, but enforce it here too: this is the
	 * predicate that decides whether a task ever looks at its signals, so a
	 * single missed strip anywhere else would otherwise make it unkillable. */
	if (sigismember_k(&sig->pending, SIGKILL) ||
	    sigismember_k(&sig->pending, SIGSTOP)) {
		return 1;
	}

	// Compute pending & ~blocked
	signandset_k(&unblocked, &sig->pending, &sig->blocked);

	return !sigisemptyset_k(&unblocked);
}

// Check if all pending signals have SA_RESTART set
// Returns 1 if syscall should be restarted (all signals have SA_RESTART)
// Returns 0 if syscall should return -EINTR (at least one signal lacks SA_RESTART)
int signal_should_restart(task_t *task)
{
	BUG_ON(task == NULL);
	if (!task)
		return 0;

	task_signal_state_t *sig = &task->signals;

	// Check all pending, unblocked signals
	for (int s = 1; s < NSIG; s++) {
		if (sigismember_k(&sig->pending, s)) {
			// Skip blocked signals (except SIGKILL/SIGSTOP)
			if (!sig_kernel_only(s) &&
			    sigismember_k(&sig->blocked, s)) {
				continue;
			}
			// Check if this signal's handler has SA_RESTART
			if (!(sig->action[s].sa_flags & SA_RESTART)) {
				return 0; // At least one signal lacks SA_RESTART
			}
		}
	}

	return 1; // All pending signals have SA_RESTART (or no pending signals)
}

// Dequeue a pending signal (returns signal number, 0 if none)
int signal_dequeue(task_t *task, kernel_sigset_t *mask, siginfo_t *info)
{
	BUG_ON(task == NULL);
	if (!task)
		return 0;

	task_signal_state_t *sig = &task->signals;
	kernel_sigset_t effective_mask;

	// Use provided mask or current blocked mask
	if (mask) {
		effective_mask = *mask;
	} else {
		effective_mask = sig->blocked;
	}

	// Find first unblocked pending signal
	// Priority: SIGKILL, SIGSTOP first, then by number
	for (int s = 1; s < NSIG; s++) {
		int signum = s;
		// Check SIGKILL/SIGSTOP first
		if (s == 1)
			signum = SIGKILL;
		else if (s == 2)
			signum = SIGSTOP;
		else if (s <= SIGKILL)
			signum = s - 2;
		else if (s <= SIGSTOP)
			signum = s - 1;
		else
			signum = s;

		// Simplified: just iterate in order
		signum = s;

		if (sigismember_k(&sig->pending, signum)) {
			// Check if blocked (SIGKILL/SIGSTOP can't be blocked)
			if (!sig_kernel_only(signum) &&
			    sigismember_k(&effective_mask, signum)) {
				continue;
			}

			// Remove from pending
			sigdelset_k(&sig->pending, signum);

			// Find and remove from queue if present
			if (info) {
				mm_memset(info, 0, sizeof(*info));
				info->si_signo = signum;
				info->si_code = SI_USER;

				pending_signal_t **pp = &sig->pending_queue;
				while (*pp) {
					if ((*pp)->sig == signum) {
						pending_signal_t *ps = *pp;
						*pp = ps->next;
						mm_memcpy(info, &ps->info,
							  sizeof(siginfo_t));
						kfree(ps);
						break;
					}
					pp = &(*pp)->next;
				}
			}

			return signum;
		}
	}

	return 0;
}

/* Bytes below the interrupted RSP that a signal frame must not touch.
 *
 * The ABI reserves the 128 bytes below %rsp -- the red zone -- for the
 * function that is running.  A leaf function may keep live data there without
 * moving %rsp, because nothing else is allowed to write below it: the CPU
 * pushes nothing on a syscall, and an interrupt switches to the kernel stack.
 *
 * A signal frame is the one thing that does write to the user stack, and it is
 * placed while that function is mid-instruction.  Starting it at RSP therefore
 * overwrites whatever the interrupted function had spilled there, and the
 * damage only shows up after the handler returns and that function reads its
 * own locals back -- as a wrong value, or a wild pointer, arbitrarily far from
 * the signal that caused it.
 *
 * So skip the reserved area first and put the frame below it.  This applies to
 * both delivery paths: the interrupt path preempts arbitrary user code, and a
 * syscall issued inline from a leaf function leaves its red zone live too,
 * since the syscall instruction pushes nothing.
 */
#define SIGFRAME_REDZONE 128

// Setup a signal frame on the user stack
// Returns 0 on success, -1 on failure
int signal_setup_frame(task_t *task, int sig, siginfo_t *info,
		       struct k_sigaction *act)
{
	BUG_ON(task == NULL || act == NULL);
	if (!task || !act)
		return -1;

	// Get current user context from task's saved syscall registers (per-task, not globals)
	uint64_t user_rsp = task->syscall_rsp;
	uint64_t user_rip = task->syscall_rip;
	uint64_t user_rflags = task->syscall_rflags;

	// Capture the handler entry point NOW, before the SA_RESETHAND reset below
	// zeroes act->sa_handler.  Reading act->sa_handler after that reset would
	// set the resume RIP to SIG_DFL (== 0) and dispatch the signal to user
	// RIP 0 — an instant SIGSEGV (exec at VA 0) instead of running the handler.
	uint64_t handler = (uint64_t)act->sa_handler;

	/* Place the signal frame so the handler is entered with the alignment
	 * the ABI promises it.
	 *
	 * The handler is entered as though by a call: RSP points AT the return
	 * address (pretcode, the first field of the frame).  At the target of a
	 * call, RSP % 16 == 8 -- 16-byte aligned before the call, minus the
	 * 8-byte return address the call pushed.  That is what the compiler
	 * assumes when it lays out the handler's own frame.
	 *
	 * Putting the frame at a 16-ALIGNED address gives the handler
	 * RSP % 16 == 0 instead, so everything it computes is 8 bytes out and
	 * the first 16-byte-aligned SSE store to the stack (`movaps %xmm0,
	 * (%rsp)`, which gcc emits for something as ordinary as initialising a
	 * local struct) raises a general protection fault -- inside the
	 * handler, with nothing to suggest the frame was misplaced.
	 *
	 * Hence: align down, then step back 8. */
	uint64_t frame_addr =
		((user_rsp - SIGFRAME_REDZONE - sizeof(signal_frame_t)) &
		 ~0xFULL) - 8;
	WARN_ON((frame_addr & 0xF) !=
		8); /* handler must be entered with RSP % 16 == 8 */

	// Validate the stack address is in user space
	if (frame_addr < 0x10000 || frame_addr >= 0x7FFFFFFFFFFF) {
		return -1;
	}

	// Build the signal frame in kernel memory first
	signal_frame_t kframe;
	mm_memset(&kframe, 0, sizeof(kframe));

	// Save all registers
	kframe.rip = user_rip;
	kframe.rsp = user_rsp;
	kframe.rflags = user_rflags;
	kframe.rbp = task->syscall_rbp;
	kframe.rbx = task->syscall_rbx;
	kframe.r12 = task->syscall_r12;
	kframe.r13 = task->syscall_r13;
	kframe.r14 = task->syscall_r14;
	kframe.r15 = task->syscall_r15;
	kframe.rcx = 0;
	kframe.rdx = 0;
	kframe.rsi = 0;
	kframe.rdi = 0;
	kframe.r8 = 0;
	kframe.r9 = 0;
	kframe.r10 = 0;
	kframe.r11 = 0;

	// Save syscall return value for sigreturn
	kframe.rax = task->syscall_rax;

	// Signal info
	kframe.sig = sig;
	if (info) {
		mm_memcpy(&kframe.info, info, sizeof(siginfo_t));
	}

	/* Save the mask sigreturn must restore.  Normally that is the current
	 * blocked set, but if ppoll()/pselect() installed a temporary mask for
	 * its wait, the caller's ORIGINAL mask is the one that has to come back
	 * after the handler — take ownership of that deferred restore here. */
	if (task->sigmask_restore_pending) {
		kframe.saved_mask = task->sigmask_saved;
		task->sigmask_restore_pending = 0;
	} else {
		kframe.saved_mask = task->signals.blocked;
	}

	// Set up sigreturn trampoline code in the frame
	// mov rax, SYS_RT_SIGRETURN (256)
	// syscall
	// This is the fallback if sa_restorer is not set
	kframe.retcode[0] = 0x48; // REX.W
	kframe.retcode[1] = 0xc7; // mov rax, imm32
	kframe.retcode[2] = 0xc0;
	kframe.retcode[3] = 0x00; // SYS_RT_SIGRETURN = 256 = 0x100
	kframe.retcode[4] = 0x01;
	kframe.retcode[5] = 0x00;
	kframe.retcode[6] = 0x00;
	kframe.retcode[7] = 0x0f; // syscall
	kframe.retcode[8] = 0x05;

	// Set return address - use sa_restorer if provided, else use embedded trampoline
	if (act->sa_restorer) {
		kframe.pretcode = (uint64_t)act->sa_restorer;
	} else {
		// Point to the retcode in the frame itself
		kframe.pretcode = frame_addr +
				  __builtin_offsetof(signal_frame_t, retcode);
	}

	// Copy frame to user stack (SMAP-aware)
	smap_disable();
	mm_memcpy((void *)frame_addr, &kframe, sizeof(kframe));
	smap_enable();

	// Update signal mask - block sa_mask and current signal (unless SA_NODEFER)
	sigorset_k(&task->signals.blocked, &task->signals.blocked,
		   &act->sa_mask);
	if (!(act->sa_flags & SA_NODEFER)) {
		sigaddset_k(&task->signals.blocked, sig);
	}
	/* A handler must not be able to make itself unkillable for its duration,
	 * via sa_mask or via being SIGKILL/SIGSTOP itself. */
	sig_strip_unblockable(&task->signals.blocked);

	// Reset handler if SA_RESETHAND
	if (act->sa_flags & SA_RESETHAND) {
		act->sa_handler = SIG_DFL;
	}

	// Save frame address in task for sigreturn to find
	task->signals.signal_frame_depth++;

	// Also update task's saved values
	task->syscall_rsp = frame_addr;
	task->syscall_rip = handler; // captured before SA_RESETHAND reset above

	// CRITICAL: Disable interrupts before modifying per-CPU syscall return context
	// This prevents a race where a timer interrupt could cause a context switch
	__asm__ volatile("cli" ::: "memory");

	// Modify the per-CPU syscall return context to call the signal handler
	// RSP = signal frame (handler should see pretcode as return address)
	// RIP = handler address
	percpu_t *cpu = this_cpu();
	cpu->syscall_user_rsp = frame_addr;
	cpu->syscall_saved_user_rip =
		handler; // captured before SA_RESETHAND reset above

	/* Republish the WHOLE return context, not just the two fields this
	 * function changes.
	 *
	 * syscall.asm's .signal_return path builds the handler's context out of
	 * per-CPU slots -- RFLAGS into R11 for sysret, and the callee-saved
	 * registers.  Those slots were written by syscall_entry on whichever
	 * CPU the call arrived on, and the syscall body then ran with
	 * interrupts ENABLED: any other task's syscall on that CPU overwrites
	 * them, and a task that migrated reads the slots of a CPU that knows
	 * nothing about it.  Only RSP and RIP were being refreshed here, so the
	 * handler was entered with whatever RFLAGS and callee-saved registers
	 * happened to be lying in the slot.
	 *
	 * When that stale RFLAGS was zero -- a CPU that had not served a
	 * syscall yet -- the handler ran with IF CLEAR, because sysret takes
	 * RFLAGS from R11 verbatim.  User code with interrupts disabled cannot
	 * be preempted and takes every fault with IRQs off, which is the
	 * "USER RIP ... with interrupts disabled (rflags=10002)" report and the
	 * might_sleep() storm behind it: 0x10002 is exactly RF (set by the CPU
	 * on the fault) plus the mandatory bit, i.e. a zero RFLAGS.
	 *
	 * task->syscall_* is the trustworthy copy: syscall_handler_inner
	 * snapshots the per-CPU values into it BEFORE enabling interrupts. */
	cpu->syscall_saved_user_rflags =
		user_rflags_sanitize(task->syscall_rflags);
	cpu->syscall_saved_user_rbp = task->syscall_rbp;
	cpu->syscall_saved_user_rbx = task->syscall_rbx;
	cpu->syscall_saved_user_r12 = task->syscall_r12;
	cpu->syscall_saved_user_r13 = task->syscall_r13;
	cpu->syscall_saved_user_r14 = task->syscall_r14;
	cpu->syscall_saved_user_r15 = task->syscall_r15;

	// Set signal pending flag - this tells syscall.asm to use signal return path
	// The value is the signal number which will be loaded into RDI
	// NOTE: Interrupts remain disabled until after sysret in syscall.asm
	cpu->syscall_signal_pending = (uint64_t)sig;

	return 0;
}

// Setup a signal frame for IRQ context (modifies IRETQ frame, not per-CPU SYSRET state)
// This is the safe variant called from timer IRQ or other interrupt handlers.
// Instead of writing to PERCPU_SIGNAL_PENDING / PERCPU_SAVED_USER_RIP etc. (which
// are only consumed by the SYSRET path in syscall.asm), we directly modify the
// interrupt_frame_t on the stack so that IRETQ returns to the signal handler.
int signal_setup_frame_irq(task_t *task, int sig, siginfo_t *info,
			   struct k_sigaction *act, interrupt_frame_t *frame)
{
	BUG_ON(task == NULL || act == NULL || frame == NULL);
	if (!task || !act || !frame)
		return -1;

	// Get current user context from the IRETQ frame (this is what the CPU
	// pushed when the interrupt fired — the real user RIP/RSP/RFLAGS)
	uint64_t user_rsp = frame->rsp;
	uint64_t user_rip = frame->rip;
	uint64_t user_rflags = frame->rflags;

	// Capture the handler entry point NOW, before the SA_RESETHAND reset below
	// zeroes act->sa_handler.  Reading it after the reset would set the IRETQ
	// frame's RIP to SIG_DFL (== 0) and return to user RIP 0 — an instant
	// SIGSEGV (exec at VA 0) instead of running the handler.
	uint64_t handler = (uint64_t)act->sa_handler;

	// Calculate new stack position for signal frame (16-byte aligned)
	/* Same alignment rule as signal_setup_frame(): the handler is entered
	 * as though by a call, so RSP must be 8 (mod 16) there. */
	uint64_t frame_addr =
		((user_rsp - SIGFRAME_REDZONE - sizeof(signal_frame_t)) &
		 ~0xFULL) - 8;
	WARN_ON((frame_addr & 0xF) != 8);

	// Validate the stack address is in user space
	if (frame_addr < 0x10000 || frame_addr >= 0x7FFFFFFFFFFF) {
		return -1;
	}

	// Build the signal frame in kernel memory first
	signal_frame_t kframe;
	mm_memset(&kframe, 0, sizeof(kframe));

	// Save all registers from the interrupt frame
	kframe.rip = user_rip;
	kframe.rsp = user_rsp;
	kframe.rflags = user_rflags;
	kframe.rbp = frame->rbp;
	kframe.rbx = frame->rbx;
	kframe.r12 = frame->r12;
	kframe.r13 = frame->r13;
	kframe.r14 = frame->r14;
	kframe.r15 = frame->r15;
	kframe.rcx = frame->rcx;
	kframe.rdx = frame->rdx;
	kframe.rsi = frame->rsi;
	kframe.rdi = frame->rdi;
	kframe.r8 = frame->r8;
	kframe.r9 = frame->r9;
	kframe.r10 = frame->r10;
	kframe.r11 = frame->r11;

	// Save RAX for sigreturn
	kframe.rax = frame->rax;

	// Signal info
	kframe.sig = sig;
	if (info) {
		mm_memcpy(&kframe.info, info, sizeof(siginfo_t));
	}

	/* Save the mask sigreturn must restore.  Normally that is the current
	 * blocked set, but if ppoll()/pselect() installed a temporary mask for
	 * its wait, the caller's ORIGINAL mask is the one that has to come back
	 * after the handler — take ownership of that deferred restore here. */
	if (task->sigmask_restore_pending) {
		kframe.saved_mask = task->sigmask_saved;
		task->sigmask_restore_pending = 0;
	} else {
		kframe.saved_mask = task->signals.blocked;
	}

	// Set up sigreturn trampoline code
	kframe.retcode[0] = 0x48; // REX.W
	kframe.retcode[1] = 0xc7; // mov rax, imm32
	kframe.retcode[2] = 0xc0;
	kframe.retcode[3] = 0x00; // SYS_RT_SIGRETURN = 256 = 0x100
	kframe.retcode[4] = 0x01;
	kframe.retcode[5] = 0x00;
	kframe.retcode[6] = 0x00;
	kframe.retcode[7] = 0x0f; // syscall
	kframe.retcode[8] = 0x05;

	// Set return address
	if (act->sa_restorer) {
		kframe.pretcode = (uint64_t)act->sa_restorer;
	} else {
		kframe.pretcode = frame_addr +
				  __builtin_offsetof(signal_frame_t, retcode);
	}

	// Copy frame to user stack
	smap_disable();
	mm_memcpy((void *)frame_addr, &kframe, sizeof(kframe));
	smap_enable();

	// Update signal mask
	sigorset_k(&task->signals.blocked, &task->signals.blocked,
		   &act->sa_mask);
	if (!(act->sa_flags & SA_NODEFER)) {
		sigaddset_k(&task->signals.blocked, sig);
	}
	/* A handler must not be able to make itself unkillable for its duration,
	 * via sa_mask or via being SIGKILL/SIGSTOP itself. */
	sig_strip_unblockable(&task->signals.blocked);

	// Reset handler if SA_RESETHAND
	if (act->sa_flags & SA_RESETHAND) {
		act->sa_handler = SIG_DFL;
	}

	// Save frame address for sigreturn
	task->signals.signal_frame_depth++;

	// Also update task's saved syscall values so sigreturn works correctly
	task->syscall_rsp = frame_addr;
	task->syscall_rip = handler; // captured before SA_RESETHAND reset above
	/* Same rule as the frame above: this is the task's record of what user
	 * mode was running with, and it is what a later signal delivery hands
	 * back. */
	task->syscall_rflags = user_rflags_sanitize(user_rflags);
	task->syscall_rbp = frame->rbp;
	task->syscall_rbx = frame->rbx;
	task->syscall_r12 = frame->r12;
	task->syscall_r13 = frame->r13;
	task->syscall_r14 = frame->r14;
	task->syscall_r15 = frame->r15;
	task->syscall_rax = frame->rax;

	// Modify the IRETQ frame directly so that when the interrupt returns
	// via iretq, execution goes to the signal handler with correct state.
	frame->rip = handler; // captured before SA_RESETHAND reset above
	frame->rsp = frame_addr; // Signal frame on user stack
	/* The handler is user code and must run like it: interrupts on, and
	 * none of the flags the interrupted context had that user mode may not
	 * choose.  This frame is IRETQ'd to directly, so what is here is what
	 * the handler runs with. */
	frame->rflags = user_rflags_sanitize(frame->rflags);
	frame->rdi = (uint64_t)sig; // First argument: signal number
	// CS, SS, RFLAGS stay the same (user mode, same flags)

	// Do NOT touch per-CPU SYSRET state — this path returns via IRETQ.

	return 0;
}

// Restore context from signal frame (called by sys_rt_sigreturn)
int signal_restore_frame(task_t *task)
{
	if (!task)
		return -1;

	/* Is this task inside a handler at all? */
	if (task->signals.signal_frame_depth == 0) {
		WARN_ON_ONCE(1); /* sigreturn with no signal frame */
		return -1;
	}

	/* WHERE the frame is: the user stack says so.
	 *
	 * The handler was entered as though by a call, with RSP pointing at
	 * pretcode -- the first field of the frame -- so its `ret' pops that
	 * and lands in the trampoline with RSP exactly 8 past the frame.  That
	 * is the frame this sigreturn belongs to, whether it is the first or
	 * the fifth one nested on this stack.
	 *
	 * Reading it from a per-task slot instead could only ever name one
	 * frame, so the inner handler's sigreturn cleared the outer's and the
	 * outer one failed -- see signal_frame_depth in struct.
	 *
	 * The alignment is the cheap check that this really is a return from a
	 * frame: setup places every frame at RSP % 16 == 8, so the trampoline
	 * always calls with RSP % 16 == 0.  Everything read out of the frame
	 * below is sanitised in any case, because it sits on the user's own
	 * stack and the user may have written it. */
	uint64_t frame_addr = task->syscall_rsp - 8;

	if ((task->syscall_rsp & 0xF) != 0 || frame_addr < 0x10000 ||
	    frame_addr >= 0x7FFFFFFFFFFF) {
		kprintf("signal_restore_frame: bad frame at 0x%lx (rsp 0x%lx)\n",
			frame_addr, task->syscall_rsp);
		return -1;
	}

	task->signals.signal_frame_depth--;

	// Read the frame from user space
	signal_frame_t kframe;
	smap_disable();
	mm_memcpy(&kframe, (void *)frame_addr, sizeof(kframe));
	smap_enable();

	// Update task's saved values first (safe without cli)
	task->syscall_rip = kframe.rip;
	task->syscall_rsp = kframe.rsp;
	/* kframe came off the user's own stack -- see user_rflags_sanitize(). */
	task->syscall_rflags = user_rflags_sanitize(kframe.rflags);
	task->syscall_rbp = kframe.rbp;
	task->syscall_rbx = kframe.rbx;
	task->syscall_r12 = kframe.r12;
	task->syscall_r13 = kframe.r13;
	task->syscall_r14 = kframe.r14;
	task->syscall_r15 = kframe.r15;

	// Save RAX (syscall return value) for assembly to restore
	task->syscall_rax = kframe.rax;

	/* Restore signal mask.  kframe was just read back from the user's own
	 * stack, so saved_mask is user-writable: a process could otherwise edit
	 * it to include SIGKILL and sigreturn itself unkillable. */
	task->signals.blocked = kframe.saved_mask;
	sig_strip_unblockable(&task->signals.blocked);

	// Clear sigsuspend flag if set
	task->signals.in_sigsuspend = 0;

	// CRITICAL: Disable interrupts before modifying per-CPU syscall return context
	// This prevents a race where a timer interrupt could cause a context switch
	__asm__ volatile("cli" ::: "memory");

	// Restore registers to per-CPU storage (output to syscall.asm)
	percpu_t *cpu = this_cpu();
	cpu->syscall_saved_user_rip = kframe.rip;
	cpu->syscall_user_rsp = kframe.rsp;
	cpu->syscall_saved_user_rflags = user_rflags_sanitize(kframe.rflags);
	cpu->syscall_saved_user_rbp = kframe.rbp;
	cpu->syscall_saved_user_rbx = kframe.rbx;
	cpu->syscall_saved_user_r12 = kframe.r12;
	cpu->syscall_saved_user_r13 = kframe.r13;
	cpu->syscall_saved_user_r14 = kframe.r14;
	cpu->syscall_saved_user_r15 = kframe.r15;
	cpu->syscall_saved_user_rax =
		kframe.rax; // Syscall return value (e.g., -EINTR)

	/* And the rest of the register file.
	 *
	 * A syscall return can leave these clobbered -- the ABI allows it -- but
	 * a signal return cannot: the signal may have interrupted user code at
	 * any instruction, with every register live.  They used to be dropped
	 * (and the assembly then zeroed them), so a signal arriving between a
	 * register load and its use corrupted the interrupted program at random.
	 * That is not a rare window: it is every instruction that is not a
	 * syscall. */
	cpu->syscall_saved_user_rdi = kframe.rdi;
	cpu->syscall_saved_user_rsi = kframe.rsi;
	cpu->syscall_saved_user_rdx = kframe.rdx;
	cpu->syscall_saved_user_rcx = kframe.rcx;
	cpu->syscall_saved_user_r8 = kframe.r8;
	cpu->syscall_saved_user_r9 = kframe.r9;
	cpu->syscall_saved_user_r10 = kframe.r10;
	cpu->syscall_saved_user_r11 = kframe.r11;

	// Tell syscall.asm to use the restored context
	// Use special value 0xFFFFFFFFFFFFFFFF (-1) to indicate sigreturn (not a handler call)
	// NOTE: Interrupts remain disabled until after sysret in syscall.asm
	cpu->syscall_signal_pending = 0xFFFFFFFFFFFFFFFFULL;

	return 0;
}

/* Record a job-control state change (stop or continue) on the task and
 * notify its parent: mark the event for waitpid(WUNTRACED/WCONTINUED),
 * send SIGCHLD with the matching CLD_ code, and wake the parent if it is
 * blocked in waitpid (same wake pattern as the exit path in
 * sched_mark_task_exited - a parent that ignores SIGCHLD must still see
 * the stopped child). */
void signal_notify_jobctl(task_t *task, int signum, int stopped)
{
	/* Record this event WITHOUT erasing the other one.
	 *
	 * These are two independent pending reports, and waitpid() clears each
	 * as it delivers it -- so clearing the opposite one here does not tidy
	 * up, it destroys an event nobody has seen yet.
	 *
	 * A continue is announced TWICE: eagerly by the killer in
	 * sched_signal_task(), and again by the target when it processes the
	 * pending SIGCONT.  That second announcement can land after a LATER
	 * stop has already been recorded -- the target's SIG_DFL_CONT case is
	 * guarded by `state == TASK_STOPPED`, which a SIGTSTP arriving in
	 * between has just made true again -- and it then wiped the fresh
	 * SIGTSTP out of jc_stop_signo.  waitpid(WUNTRACED) had nothing to
	 * report for a child that really had stopped: the
	 * "SIGTSTP stop reported via WUNTRACED" case, roughly one run in a
	 * hundred. */
	if (stopped)
		task->jc_stop_signo = signum;
	else
		task->jc_continued = 1;
	task_t *parent = task->parent;
	if (!parent || parent->has_exited)
		return;
	siginfo_t ci;
	mm_memset(&ci, 0, sizeof(ci));
	ci.si_signo = SIGCHLD;
	ci.si_code = stopped ? CLD_STOPPED : CLD_CONTINUED;
	ci.si_pid = task->id;
	ci.si_status = signum;
	signal_send(parent, SIGCHLD, &ci);
	/* Wake a parent blocked in waitpid (wait_channel == itself) via the
	 * claim CAS; a spurious wake is safe, the waitpid loop rechecks.
	 *
	 * The look at the parent goes under g_wait_lock for the same reason the
	 * exit path's does: the jc_ fields above are set before we take it, so
	 * a parent deciding to sleep right now either sees them and stays
	 * awake, or is already BLOCKED here and gets woken.  Without that,
	 * WUNTRACED/WCONTINUED waits lose the wake and hang — the same trap as
	 * the exit notification, and this one has no signal to fall back on
	 * either.
	 *
	 * Every thread of the parent process: children hang off the group
	 * leader and any thread of the group may reap them, so the sleeper is
	 * often not the task recorded as the parent. */
	sched_wake_wait_sleepers(parent);
}

// Deliver pending signals to a task (called before returning to userspace)
/*
 * Queue a synchronous fault signal (SIGSEGV/SIGILL/SIGBUS/SIGFPE/SIGTRAP).
 *
 * Two things make a fault different from a signal somebody sent, and both are
 * handled here rather than at the delivery point:
 *
 *   - The disposition is FORCED back to default if the program has blocked or
 *     ignored the signal.  A fault is not an event that can be declined: the
 *     faulting instruction has not completed, so returning to it without
 *     acting faults again, identically, for ever.  Resetting to SIG_DFL and
 *     unblocking is what every other system does with a synchronous fault, and
 *     it is what turns an unkillable spin into an ordinary crash.
 *
 *   - The siginfo carries si_code and si_addr.  A debugger reports these -- the
 *     difference between "it crashed" and "it wrote to 0x1" -- so they are
 *     filled in at the fault, the only place that knows them.
 *
 * The signal is left PENDING.  Nothing is decided here: no stop, no kill, no
 * report.  The return-to-user path picks it up, which is the only context
 * where a tracer can be consulted.
 */
void signal_force_fault(task_t *task, int sig, int code, uint64_t addr)
{
	siginfo_t info;

	if (!task || sig <= 0 || sig >= NSIG)
		return;

	struct k_sigaction *act = &task->signals.action[sig];

	if (act->sa_handler == SIG_IGN ||
	    (act->sa_handler == SIG_DFL &&
	     sig_default_action(sig) == SIG_DFL_IGN)) {
		act->sa_handler = SIG_DFL;
		act->sa_flags = 0;
	}
	sigdelset_k(&task->signals.blocked, sig);

	mm_memset(&info, 0, sizeof(info));
	info.si_signo = sig;
	info.si_code = code;
	info.si_addr = (void *)addr;

	signal_send(task, sig, &info);
}

/*
 * Choose what this task should do about its next pending signal, and do all of
 * it except arranging a user handler's stack frame.
 *
 * Returns the signal whose handler the caller must now set up, or 0 when there
 * is nothing left to arrange -- the queue is empty, or the signal was ignored,
 * discarded by a tracer, or already acted on here (stopped, continued, killed).
 *
 * THE LOOP IS THE POINT.  A traced task stops so its tracer can choose, and the
 * tracer may choose nothing at all; when that happens the next pending signal
 * has to be examined rather than returning to user space.  Returning instead
 * was the defect that made an earlier attempt at this strand a process: the
 * tracee had been released from its stop by a SIGKILL, the suppressed fault
 * sent it back to the faulting instruction, and the SIGKILL sat in the queue
 * unlooked-at while the task faulted, stopped and resumed for ever.  Coming
 * round the loop finds that SIGKILL and takes the ordinary fatal path.
 *
 * MAY SLEEP.  The stop parks the caller, so this is only callable from a
 * return-to-user path with interrupts on and no locks held.
 */
static int signal_select(task_t *task, siginfo_t *info)
{
	task_signal_state_t *sig = &task->signals;

	for (;;) {
		int signum = signal_dequeue(task, NULL, info);

		if (signum == 0)
			return 0;

		/* The tracer sees it first and says what should happen: this
		 * signal, a different one, or none.  Untraced tasks get their
		 * own signal back unchanged. */
		signum = task_ptrace_signal_stop(task, signum, info);
		if (signum == 0)
			continue;

		/* Re-read the disposition: the tracer may have substituted a
		 * different signal, and a stop is long enough for the program
		 * itself to be a different program (the tracer can write its
		 * memory and registers while it is parked). */
		struct k_sigaction *act = &sig->action[signum];

		if (act->sa_handler == SIG_IGN)
			continue;

		if (act->sa_handler != SIG_DFL)
			return signum; /* caller arranges the handler */

		switch (sig_default_action(signum)) {
		case SIG_DFL_TERM:
		case SIG_DFL_CORE:
			/* The whole process, not just this thread: a fatal
			 * default action ends every thread of the group, or
			 * abort() in one thread leaves the others running and
			 * the program hangs instead of dying. */
			task->term_sig = signum;
			sched_kill_thread_group(task, 128 + signum);
			sched_mark_task_exited(task, 128 + signum);
			return 0;
		case SIG_DFL_STOP:
			/* The stop is entered HERE, by the task itself, and
			 * nowhere else.  Marking the state was not enough: this
			 * returns to the delivery path, which pops the frame and
			 * IRETs back to user code, so a task "stopped" from here
			 * carried on running with TASK_STOPPED written on it.
			 * The whole stop -- leaving the run queue, the state,
			 * the parent's event and giving up the CPU -- is one
			 * step, and sched_do_signal_stop() is it. */
			sched_do_signal_stop(task, signum);
			return 0;
		case SIG_DFL_CONT:
			if (task->state == TASK_STOPPED) {
				task->state = TASK_READY;
				signal_notify_jobctl(task, signum, 0);
			}
			continue;
		case SIG_DFL_IGN:
		default:
			continue;
		}
	}
}

void signal_deliver(task_t *task)
{
	if (!task || task->privilege != TASK_USER)
		return;
	WARN_ON(task->state ==
		TASK_ZOMBIE); /* signal delivery to zombie task */

	siginfo_t info;
	int signum = signal_select(task, &info);

	if (signum == 0)
		return; /* nothing to arrange -- see signal_select */

	/* User-defined handler: set up the signal frame. */
	if (signal_setup_frame(task, signum, &info,
			       &task->signals.action[signum]) < 0) {
		/* Failed to set up the frame -- terminate with the signal.
		 * The whole process, for the same reason as the fatal default
		 * action above: a thread group whose leader is a zombie while
		 * its threads still run is never reported finished to its
		 * parent. */
		task->term_sig = signum;
		sched_kill_thread_group(task, 128 + signum);
		sched_mark_task_exited(task, 128 + signum);
	}
}

// Deliver pending signals via IRETQ frame (called from timer IRQ / interrupt context)
// This is the interrupt-safe variant of signal_deliver().  Instead of writing
// per-CPU SYSRET state (PERCPU_SIGNAL_PENDING etc.), it modifies the IRETQ
// frame on the interrupt stack so that the interrupt return goes directly to
// the signal handler in user space.
void signal_deliver_irq(task_t *task, interrupt_frame_t *frame)
{
	if (!task || !frame || task->privilege != TASK_USER)
		return;

	// Skip signal delivery for zombie/exited tasks.  A cross-CPU kill()
	// may have already marked this task as zombie via sched_mark_task_exited
	// while SIGKILL is still pending in the signal queue.  Re-processing
	// the signal would call sched_mark_task_exited a second time, causing
	// double resource release and state corruption.
	if (task->has_exited || task->state == TASK_ZOMBIE)
		return;

	siginfo_t info;
	int signum = signal_select(task, &info);

	if (signum == 0) {
		/* Nothing to arrange.  If signal_select ended the task, make
		 * sure the return path reschedules rather than IRETing back
		 * into a process that no longer exists. */
		if (task->has_exited || task->state == TASK_ZOMBIE)
			sched_set_need_resched(task);
		return;
	}

	// User-defined handler — modify the IRETQ frame
	if (signal_setup_frame_irq(task, signum, &info,
				   &task->signals.action[signum], frame) < 0) {
		/* The whole process: see signal_deliver(). */
		task->term_sig = signum;
		sched_kill_thread_group(task, 128 + signum);
		sched_mark_task_exited(task, 128 + signum);
	}
}

// Check and fire interval timers (called from timer tick)
void signal_check_timers(task_t *task, uint64_t current_tick)
{
	if (!task)
		return;

	task_signal_state_t *sig = &task->signals;

	// Check alarm
	if (sig->alarm_ticks > 0 && current_tick >= sig->alarm_ticks) {
		sig->alarm_ticks = 0;
		siginfo_t info;
		mm_memset(&info, 0, sizeof(info));
		info.si_signo = SIGALRM;
		info.si_code = SI_TIMER;
		signal_send(task, SIGALRM, &info);
	}

	// Check ITIMER_REAL
	if (sig->itimer_real.it_value.tv_sec > 0 ||
	    sig->itimer_real.it_value.tv_usec > 0) {
		// Decrement timer (simplified: assume 10ms per tick at 100Hz)
		int64_t usec = sig->itimer_real.it_value.tv_usec - 10000;
		if (usec < 0) {
			sig->itimer_real.it_value.tv_sec--;
			usec += 1000000;
		}
		sig->itimer_real.it_value.tv_usec = usec;

		if (sig->itimer_real.it_value.tv_sec <= 0 &&
		    sig->itimer_real.it_value.tv_usec <= 0) {
			// Timer expired
			siginfo_t info;
			mm_memset(&info, 0, sizeof(info));
			info.si_signo = SIGALRM;
			info.si_code = SI_TIMER;
			signal_send(task, SIGALRM, &info);

			// Reload if interval set
			if (sig->itimer_real.it_interval.tv_sec > 0 ||
			    sig->itimer_real.it_interval.tv_usec > 0) {
				sig->itimer_real.it_value =
					sig->itimer_real.it_interval;
			}
		}
	}
}

// POSIX timer functions

ktimer_t timer_create_internal(task_t *task, clockid_t clockid,
			       struct k_sigevent *sevp)
{
	if (!task)
		return -1;

	// Find free slot
	int slot = -1;
	for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
		if (!g_posix_timers[i].in_use) {
			slot = i;
			break;
		}
	}

	if (slot < 0)
		return -1;

	kernel_timer_t *kt = &g_posix_timers[slot];
	kt->in_use = 1;
	kt->timerid = g_next_timerid++;
	kt->clockid = clockid;
	kt->owner_pid = task->id;
	kt->overrun = 0;
	kt->next_tick = 0;
	kt->interval_ticks = 0;

	if (sevp) {
		mm_memcpy(&kt->sevp, sevp, sizeof(struct k_sigevent));
	} else {
		// Default: SIGEV_SIGNAL with SIGALRM
		kt->sevp.sigev_notify = SIGEV_SIGNAL;
		kt->sevp.sigev_signo = SIGALRM;
	}

	mm_memset(&kt->spec, 0, sizeof(kt->spec));

	return kt->timerid;
}

int timer_settime_internal(ktimer_t timerid, int flags,
			   const struct k_itimerspec *new_value,
			   struct k_itimerspec *old_value)
{
	(void)flags; // TODO: handle TIMER_ABSTIME

	// Find timer
	kernel_timer_t *kt = NULL;
	for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
		if (g_posix_timers[i].in_use &&
		    g_posix_timers[i].timerid == timerid) {
			kt = &g_posix_timers[i];
			break;
		}
	}

	if (!kt)
		return -EINVAL;

	if (old_value) {
		mm_memcpy(old_value, &kt->spec, sizeof(struct k_itimerspec));
	}

	if (new_value) {
		mm_memcpy(&kt->spec, new_value, sizeof(struct k_itimerspec));

		// Calculate next expiration in ticks using measured frequency
		uint32_t freq = timer_get_frequency();
		uint64_t current = timer_ticks();
		uint64_t nsec = new_value->it_value.tv_sec * 1000000000ULL +
				new_value->it_value.tv_nsec;
		uint64_t ticks = nsec * freq / 1000000000ULL;
		kt->next_tick = current + ticks;

		// Calculate interval
		nsec = new_value->it_interval.tv_sec * 1000000000ULL +
		       new_value->it_interval.tv_nsec;
		kt->interval_ticks = nsec * freq / 1000000000ULL;

		kt->overrun = 0;
	}

	return 0;
}

int timer_gettime_internal(ktimer_t timerid, struct k_itimerspec *curr_value)
{
	kernel_timer_t *kt = NULL;
	for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
		if (g_posix_timers[i].in_use &&
		    g_posix_timers[i].timerid == timerid) {
			kt = &g_posix_timers[i];
			break;
		}
	}

	if (!kt)
		return -EINVAL;

	if (curr_value) {
		// Calculate remaining time
		uint64_t current = timer_ticks();
		if (kt->next_tick > current) {
			uint64_t remaining =
				(kt->next_tick - current) *
				(1000000000ULL / timer_get_frequency());
			curr_value->it_value.tv_sec = remaining / 1000000000ULL;
			curr_value->it_value.tv_nsec =
				remaining % 1000000000ULL;
		} else {
			curr_value->it_value.tv_sec = 0;
			curr_value->it_value.tv_nsec = 0;
		}
		curr_value->it_interval = kt->spec.it_interval;
	}

	return 0;
}

int timer_getoverrun_internal(ktimer_t timerid)
{
	for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
		if (g_posix_timers[i].in_use &&
		    g_posix_timers[i].timerid == timerid) {
			return g_posix_timers[i].overrun;
		}
	}
	return -EINVAL;
}

int timer_delete_internal(ktimer_t timerid)
{
	for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
		if (g_posix_timers[i].in_use &&
		    g_posix_timers[i].timerid == timerid) {
			g_posix_timers[i].in_use = 0;
			return 0;
		}
	}
	return -EINVAL;
}

// Check and fire POSIX timers (called from timer tick)
void signal_check_posix_timers(uint64_t current_tick)
{
	for (int i = 0; i < MAX_POSIX_TIMERS; i++) {
		kernel_timer_t *kt = &g_posix_timers[i];
		if (!kt->in_use)
			continue;
		if (kt->next_tick == 0)
			continue;

		if (current_tick >= kt->next_tick) {
			// Timer expired
			task_t *owner = sched_find_task_by_id(kt->owner_pid);
			if (owner && kt->sevp.sigev_notify == SIGEV_SIGNAL) {
				siginfo_t info;
				mm_memset(&info, 0, sizeof(info));
				info.si_signo = kt->sevp.sigev_signo;
				info.si_code = SI_TIMER;
				info.si_timerid = kt->timerid;
				info.si_overrun = kt->overrun;
				signal_send(owner, kt->sevp.sigev_signo, &info);
			}

			// Reload or disarm
			if (kt->interval_ticks > 0) {
				// Count overruns
				while (kt->next_tick <= current_tick) {
					kt->next_tick += kt->interval_ticks;
					kt->overrun++;
				}
				kt->overrun--; // First expiration isn't an overrun
			} else {
				kt->next_tick = 0; // Disarm
			}
		}
	}
}

static void kill_task(task_t *t, int sig)
{
	if (!t) {
		return;
	}
	// Use sched_signal_task which properly handles SIGKILL/SIGSTOP
	// and other signals with their default actions
	sched_signal_task(t, sig);
}

int64_t sys_kill(uint64_t pid, uint64_t sig)
{
	if (sig > 64)
		return -EINVAL;
	task_t *self = sched_current();
	if (!self)
		return -EFAULT;
	/* POSIX pid forms:
	 *   pid  > 0   that process
	 *   pid == 0   every process in the CALLER's process group
	 *   pid <  -1  every process in process group -pid
	 * pid 0 used to be refused as "the kernel idle task", but 0 is not a
	 * pid here at all — it is the caller's own group.  A shell relies on
	 * this: when it finds itself in the background it does kill(0, SIGTTIN)
	 * to stop until it is moved to the foreground, and the EPERM made it
	 * spin and then switch job control off entirely. */
	if (pid == 0) {
		if (self->pgid <= 0)
			return -ESRCH;
		/* Our own group always contains us and we may always signal
		 * ourselves, so this cannot come back -EPERM; the check only
		 * skips members belonging to another user, which a group can
		 * acquire across a setuid exec. */
		return sched_signal_pgrp_checked(self->pgid, (int)sig);
	}
	if ((int64_t)pid < -1) {
		int64_t pgid = -(int64_t)pid;
		if (pgid > 0x7fffffff)
			return -ESRCH;
		return sched_signal_pgrp_checked((int)pgid, (int)sig);
	}
	if ((int64_t)pid == -1) {
		/* Broadcast: every process the caller may signal, except itself
		 * and init.  Treating -1 as "process group 1" (which is what
		 * negating it used to produce) signalled init's group instead
		 * of everything, which is both wrong and dangerous. */
		if (sig == 0)
			return 0;
		return sched_signal_all(self, (int)sig);
	}
	task_t *t = sched_find_task_by_id((uint32_t)pid);
	if (!t)
		return -ESRCH;
	// Kernel tasks (idle, init, kernel threads) cannot be signalled
	if (t->privilege == TASK_KERNEL)
		return -EPERM;
	/* Credential check: an unprivileged caller may only signal a process
	 * with a matching uid (applies even to the sig==0 existence probe). */
	int perr = signal_permission(t, (int)sig);
	if (perr != 0)
		return perr;
	if (sig == 0)
		return 0;
	kill_task(t, (int)sig);
	return 0;
}

// ============================================================================
// Signal Syscalls
// ============================================================================

// SYS_RT_SIGACTION - set signal handler
int64_t sys_rt_sigaction(uint64_t sig, uint64_t act_ptr,
			 uint64_t oldact_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}
	if (sig <= 0 || sig >= NSIG) {
		return -EINVAL;
	}
	if (sig_kernel_only(sig)) {
		return -EINVAL; // Can't change SIGKILL/SIGSTOP
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	struct k_sigaction *kact = &cur->signals.action[sig];

	// Copy old action if requested
	if (oldact_ptr) {
		if (copy_to_user((void *)oldact_ptr, kact,
				 sizeof(struct k_sigaction)) != 0) {
			return -EFAULT;
		}
	}

	// Set new action if provided
	if (act_ptr) {
		struct k_sigaction newact;
		if (copy_from_user(&newact, (void *)act_ptr,
				   sizeof(struct k_sigaction)) != 0) {
			return -EFAULT;
		}
		/* sa_mask is applied to the blocked mask for the duration of the
		 * handler.  Strip the unblockable signals here, at the point they
		 * enter the kernel, so a filled sa_mask cannot make the task
		 * unkillable while its handler runs. */
		sig_strip_unblockable(&newact.sa_mask);
		mm_memcpy(kact, &newact, sizeof(struct k_sigaction));
	}

	return 0;
}

// SYS_RT_SIGPROCMASK - change blocked signals
int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_ptr,
			   uint64_t oldset_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	kernel_sigset_t *blocked = &cur->signals.blocked;

	// Copy old mask if requested
	if (oldset_ptr) {
		if (copy_to_user((void *)oldset_ptr, blocked,
				 sizeof(kernel_sigset_t)) != 0) {
			return -EFAULT;
		}
	}

	// Set new mask if provided
	if (set_ptr) {
		kernel_sigset_t newset;
		if (copy_from_user(&newset, (void *)set_ptr,
				   sizeof(kernel_sigset_t)) != 0) {
			return -EFAULT;
		}

		switch (how) {
		case SIG_BLOCK:
			sigorset_k(blocked, blocked, &newset);
			break;
		case SIG_UNBLOCK:
			signandset_k(blocked, blocked, &newset);
			break;
		case SIG_SETMASK:
			*blocked = newset;
			break;
		default:
			return -EINVAL;
		}

		// Can't block SIGKILL or SIGSTOP
		sig_strip_unblockable(blocked);
	}

	return 0;
}

// SYS_RT_SIGPENDING - get pending signals
int64_t sys_rt_sigpending(uint64_t set_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	if (copy_to_user((void *)set_ptr, &cur->signals.pending,
			 sizeof(kernel_sigset_t)) != 0) {
		return -EFAULT;
	}

	return 0;
}

// SYS_RT_SIGTIMEDWAIT - wait for signal with timeout
int64_t sys_rt_sigtimedwait(uint64_t set_ptr, uint64_t info_ptr,
			    uint64_t timeout_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	kernel_sigset_t wait_set;
	if (copy_from_user(&wait_set, (void *)set_ptr,
			   sizeof(kernel_sigset_t)) != 0) {
		return -EFAULT;
	}

	struct k_timespec timeout;
	uint64_t deadline = 0;
	if (timeout_ptr) {
		if (copy_from_user(&timeout, (void *)timeout_ptr,
				   sizeof(struct k_timespec)) != 0) {
			return -EFAULT;
		}
		uint32_t freq = timer_get_frequency();
		uint64_t ticks =
			timeout.tv_sec * freq +
			(uint64_t)timeout.tv_nsec * freq / 1000000000ULL;
		deadline = timer_ticks() + ticks;
	}

	// Check if any signals in wait_set are already pending
	while (1) {
		for (int sig = 1; sig < NSIG; sig++) {
			if (sigismember_k(&wait_set, sig) &&
			    sigismember_k(&cur->signals.pending, sig)) {
				// Found a signal
				siginfo_t info;
				signal_dequeue(cur, &wait_set, &info);

				if (info_ptr) {
					if (copy_to_user(
						    (void *)info_ptr, &info,
						    sizeof(siginfo_t)) != 0) {
						return -EFAULT;
					}
				}
				return sig;
			}
		}

		// Check timeout
		if (timeout_ptr && timer_ticks() >= deadline) {
			return -EAGAIN;
		}

		// Block task and wait
		cur->state = TASK_BLOCKED;
		sched_schedule();

		// Check if we should exit
		if (cur->has_exited) {
			return -EINTR;
		}
	}
}

/* Shared gate for the signal syscalls that reach a task straight from its id
 * (rt_sigqueueinfo, tkill, tgkill).  Without it they bypass both guards
 * sys_kill applies: kernel threads are not signallable at all, and an
 * unprivileged caller may only signal a task whose credentials match
 * (signal_permission()).  sig == 0 is the probe form, so the check runs for it
 * too — that probe IS the permission answer. */
int64_t signal_target_check(task_t *target, int sig)
{
	if (target->privilege == TASK_KERNEL)
		return -EPERM;
	return signal_permission(target, sig);
}

// SYS_RT_SIGQUEUEINFO - queue signal with info
int64_t sys_rt_sigqueueinfo(uint64_t pid, uint64_t sig,
			    uint64_t info_ptr)
{
	if (sig <= 0 || sig >= NSIG) {
		return -EINVAL;
	}

	task_t *target = sched_find_task_by_id((uint32_t)pid);
	if (!target) {
		return -ESRCH;
	}

	int64_t perr = signal_target_check(target, (int)sig);
	if (perr != 0)
		return perr;

	siginfo_t info;
	if (copy_from_user(&info, (void *)info_ptr, sizeof(siginfo_t)) != 0) {
		return -EFAULT;
	}

	// Enforce that si_code indicates user-originated
	info.si_code = SI_QUEUE;

	return signal_send(target, (int)sig, &info);
}

// SYS_RT_SIGSUSPEND - suspend until signal
int64_t sys_rt_sigsuspend(uint64_t mask_ptr, uint64_t sigsetsize)
{
	if (sigsetsize != sizeof(kernel_sigset_t)) {
		return -EINVAL;
	}

	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	kernel_sigset_t newmask;
	if (copy_from_user(&newmask, (void *)mask_ptr,
			   sizeof(kernel_sigset_t)) != 0) {
		return -EFAULT;
	}

	// Save current mask and set new one
	cur->signals.saved_mask = cur->signals.blocked;
	cur->signals.blocked = newmask;
	cur->signals.in_sigsuspend = 1;

	// Can't block SIGKILL/SIGSTOP
	sig_strip_unblockable(&cur->signals.blocked);

	// Block until signal
	cur->state = TASK_BLOCKED;

	while (!signal_pending(cur)) {
		sched_schedule();
		if (cur->has_exited) {
			cur->signals.in_sigsuspend = 0;
			cur->signals.blocked = cur->signals.saved_mask;
			return -EINTR;
		}
	}

	// Restore mask
	cur->signals.in_sigsuspend = 0;
	cur->signals.blocked = cur->signals.saved_mask;

	return -EINTR; // sigsuspend always returns EINTR
}

// SYS_RT_SIGRETURN - return from signal handler
int64_t sys_rt_sigreturn(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	// Restore context from the signal frame
	if (signal_restore_frame(cur) < 0) {
		kprintf("sys_rt_sigreturn: failed to restore frame\n");
		return -EFAULT;
	}

	// The return value will be ignored - we're restoring the original
	// context which includes the original RAX value
	return 0;
}

// SYS_SIGALTSTACK - set/get alternate signal stack
int64_t sys_sigaltstack(uint64_t ss_ptr, uint64_t old_ss_ptr)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	// Copy old stack if requested
	if (old_ss_ptr) {
		if (copy_to_user((void *)old_ss_ptr, &cur->signals.altstack,
				 sizeof(stack_t)) != 0) {
			return -EFAULT;
		}
	}

	// Set new stack if provided
	if (ss_ptr) {
		stack_t newss;
		if (copy_from_user(&newss, (void *)ss_ptr, sizeof(stack_t)) !=
		    0) {
			return -EFAULT;
		}

		// Validate
		if (!(newss.ss_flags & SS_DISABLE)) {
			if (newss.ss_size < MINSIGSTKSZ) {
				return -ENOMEM;
			}
		}

		cur->signals.altstack = newss;
	}

	return 0;
}

// SYS_TKILL - send signal to specific thread
int64_t sys_tkill(uint64_t tid, uint64_t sig)
{
	// sig == 0 is the existence/permission probe; only negatives are errors
	if ((int64_t)tid <= 0 || (int64_t)sig < 0 || sig >= NSIG) {
		return -EINVAL;
	}

	task_t *target = sched_find_task_by_id((uint32_t)tid);
	if (!target) {
		return -ESRCH;
	}

	int64_t perr = signal_target_check(target, (int)sig);
	if (perr != 0)
		return perr;

	if (sig == 0) {
		return 0;
	}

	task_t *self = sched_current();
	siginfo_t info;
	mm_memset(&info, 0, sizeof(info));
	info.si_signo = (int)sig;
	info.si_code = SI_TKILL;
	// si_pid is the sending *process*, i.e. the sender's tgid, not its tid
	info.si_pid = self ? self->tgid : 0;
	info.si_uid = self ? self->cred.uid : 0;

	return signal_send(target, (int)sig, &info);
}

// SYS_TGKILL - send signal to thread in specific thread group
// This is the secure way to send signals to threads - validates that
// the target thread belongs to the specified thread group.
int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig)
{
	if ((int64_t)tgid <= 0 || (int64_t)tid <= 0) {
		return -EINVAL;
	}

	if ((int64_t)sig < 0 || sig >= NSIG) {
		return -EINVAL;
	}

	// Find the target thread
	task_t *target = sched_find_task_by_id((uint32_t)tid);
	if (!target) {
		return -ESRCH;
	}

	// Validate that target belongs to the specified thread group
	if (target->tgid != (int)tgid) {
		return -ESRCH; // Thread not in specified group
	}

	int64_t perr = signal_target_check(target, (int)sig);
	if (perr != 0)
		return perr;

	// sig == 0 is a permission check only
	if (sig == 0) {
		return 0;
	}

	task_t *self = sched_current();
	// Build siginfo
	siginfo_t info;
	mm_memset(&info, 0, sizeof(info));
	info.si_signo = (int)sig;
	info.si_code = SI_TKILL;
	info.si_pid = self ? self->tgid : 0;
	info.si_uid = self ? self->cred.uid : 0;

	return signal_send(target, (int)sig, &info);
}

// SYS_PAUSE - suspend until signal
int64_t sys_pause(void)
{
	task_t *cur = sched_current();
	if (!cur)
		return -EFAULT;

	// Block until any signal arrives
	cur->state = TASK_BLOCKED;

	while (!signal_pending(cur)) {
		sched_schedule();
		if (cur->has_exited) {
			return -EINTR;
		}
	}

	return -EINTR; // pause always returns EINTR
}

// SYS_SIGNALFD / SYS_SIGNALFD4 - create signalfd (simplified stub)
int64_t sys_signalfd(uint64_t fd, uint64_t mask_ptr, uint64_t flags)
{
	(void)fd;
	(void)mask_ptr;
	(void)flags;
	// signalfd is complex to implement fully - return ENOSYS for now
	return -ENOSYS;
}
