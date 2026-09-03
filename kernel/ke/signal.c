// LikeOS-64 Kernel Signal Implementation
#include <kernel/dev/device.h>
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
#include <kernel/ke/waitq.h>

// NOTE: Signal delivery now uses per-CPU storage via percpu_t
/* Bytes below RSP the ABI reserves for leaf functions (the red zone); a
 * signal frame is placed below it.  Rationale with the frame builders. */
#define SIGFRAME_REDZONE 128

/* Serialises every task's queued-siginfo list.
 *
 * The pending BITMASK is handled without a lock, by the atomic operations in
 * signal.h.  The queued descriptions cannot be: they are a linked list, and a
 * sender pushing a node while the owning task unlinks one either loses the
 * push or leaves the pushed node pointing at memory that was just freed.
 *
 * One lock for all tasks rather than one per task, deliberately.  The
 * critical sections are a list push and a walk over at most one node per
 * signal number, so there is nothing to contend for; and a per-task lock
 * lives in the task, which is exactly what a previous per-task lock here got
 * wrong -- a lock in memory that had been freed under a second owner left the
 * next sender spinning forever with interrupts disabled, which presents as a
 * machine that simply stopped.  A file-static lock cannot be freed.
 *
 * It is a LEAF lock: nothing is called while it is held.  In particular the
 * allocation happens before it and kfree() after it, because kfree() can
 * raise a TLB shootdown IPI and that must not happen with interrupts off.
 */
static spinlock_t g_sigqueue_lock = SPINLOCK_INIT("sigqueue");

// The old global syscall_signal_pending is deprecated.

/* The interrupted context's extended register state travels in the signal
 * frame (see signal_frame_t): the x87/SSE/AVX registers are caller-saved in
 * the ABI, and a handler is a caller the interrupted code never made, so
 * without this the first vector-using libc call inside a handler clobbers
 * registers the interrupted computation still owns.
 *
 * At both delivery points (syscall tail and IRQ tail) the hardware registers
 * hold the CURRENT task's user state -- the context switch saves and
 * restores it around every migration -- so the image is taken straight from
 * the registers into the user frame.  The frame's image slot is 64-byte
 * aligned (sigframe_fpu_addr), which XSAVE requires. */
static void sigframe_fpu_capture(uint64_t fpu_addr)
{
	smap_disable();
	fpu_save((void *)fpu_addr);
	smap_enable();
}

/* The reverse, at sigreturn.  The image sits on the user's own stack and is
 * user-writable, and a poisoned one makes the restore instruction fault in
 * the kernel, so it is copied into the task's own save area, sanitised
 * there, and loaded from there.  Interrupts are off across the three steps:
 * a context switch in between would overwrite the save area with the live
 * (handler's) registers and the wrong state would be loaded. */
static void sigframe_fpu_restore(task_t *task, uint64_t fpu_addr)
{
	uint64_t irqf = local_irq_save();

	smap_disable();
	mm_memcpy(task->fpu_state, (const void *)fpu_addr, g_fpu_state_size);
	smap_enable();
	fpu_sanitize_state(task->fpu_state);
	fpu_restore(task->fpu_state);
	local_irq_restore(irqf);
}

/* ---- Frame placement and construction, shared by both delivery paths ----
 *
 * Choose where the frame goes (the alternate stack if the action asks for
 * one and we are not already on it, else below the interrupted RSP, past
 * the red zone), and give both addresses the alignment the ABI and XSAVE
 * demand: the handler is entered as though by a call, so RSP % 16 must be
 * 8 there (the compiler lays out the handler's frame on that assumption,
 * and the first `movaps' to a stack local faults otherwise), and the
 * register image must sit on a 64-byte boundary. */
static int sigframe_place(task_t *task, const struct k_sigaction *act,
			  uint64_t user_rsp, uint64_t *frame_addr_out,
			  uint64_t *fpu_addr_out, int *on_altstack_out)
{
	const stack_t *alt = &task->signals.altstack;
	uint64_t sp = user_rsp - SIGFRAME_REDZONE;
	int on_alt = 0;

	if (alt->ss_sp && alt->ss_size && !(alt->ss_flags & SS_DISABLE)) {
		uint64_t lo = (uint64_t)alt->ss_sp;
		uint64_t hi = lo + alt->ss_size;

		if (user_rsp >= lo && user_rsp < hi) {
			on_alt = 1; /* already on it: nest below */
		} else if (act->sa_flags & SA_ONSTACK) {
			sp = hi; /* switch: no red zone on a fresh stack */
			on_alt = 1;
		}
	}

	uint64_t fpu_addr = (sp - SIGFRAME_FPU_SIZE) & ~63ULL;
	uint64_t frame_addr =
		((fpu_addr - sizeof(signal_frame_t)) & ~0xFULL) - 8;

	/* sigframe_fpu_addr() must land back on the slot chosen here. */
	WARN_ON(sigframe_fpu_addr(frame_addr) != fpu_addr);
	WARN_ON((frame_addr & 0xF) != 8);

	if (frame_addr < 0x10000 || frame_addr >= 0x7FFFFFFFFFFF)
		return -1;
	*frame_addr_out = frame_addr;
	*fpu_addr_out = fpu_addr;
	*on_altstack_out = on_alt;
	return 0;
}

/* Fill in everything in the frame that is not a register: identity,
 * siginfo, the mask sigreturn restores, the altstack description, the
 * trampoline, the return address. */
static void sigframe_fill(signal_frame_t *kf, task_t *task, int sig,
			  const siginfo_t *info, const struct k_sigaction *act,
			  uint64_t frame_addr, uint64_t fpu_addr, int on_alt)
{
	kf->sig = sig;
	if (info)
		mm_memcpy(&kf->info, info, sizeof(siginfo_t));

	/* Save the mask sigreturn must restore.  Normally that is the current
	 * blocked set, but if ppoll()/pselect() installed a temporary mask
	 * for its wait, the caller's ORIGINAL mask is the one that has to
	 * come back after the handler -- take ownership of that deferred
	 * restore here. */
	if (task->sigmask_restore_pending) {
		kf->uc.uc_sigmask = task->sigmask_saved;
		task->sigmask_restore_pending = 0;
	} else {
		kf->uc.uc_sigmask = task->signals.blocked;
	}

	kf->uc.uc_flags = 0;
	kf->uc.uc_link = NULL;
	kf->uc.uc_stack = task->signals.altstack;
	kf->uc.uc_stack.ss_flags =
		(kf->uc.uc_stack.ss_flags & SS_DISABLE) ? SS_DISABLE :
		on_alt					 ? SS_ONSTACK :
							   0;
	kf->uc.uc_mcontext.fpregs = (void *)fpu_addr;

	/* Sigreturn trampoline: mov $SYS_RT_SIGRETURN, %rax ; syscall.
	 * The fallback when sa_restorer is not set. */
	kf->retcode[0] = 0x48; // REX.W
	kf->retcode[1] = 0xc7; // mov rax, imm32
	kf->retcode[2] = 0xc0;
	kf->retcode[3] = 0x00; // SYS_RT_SIGRETURN = 256 = 0x100
	kf->retcode[4] = 0x01;
	kf->retcode[5] = 0x00;
	kf->retcode[6] = 0x00;
	kf->retcode[7] = 0x0f; // syscall
	kf->retcode[8] = 0x05;

	if (act->sa_restorer)
		kf->pretcode = (uint64_t)act->sa_restorer;
	else
		kf->pretcode = frame_addr +
			       __builtin_offsetof(signal_frame_t, retcode);
}

/* Copy the frame out and capture the register image behind it. */
static void sigframe_commit(const signal_frame_t *kf, uint64_t frame_addr,
			    uint64_t fpu_addr)
{
	smap_disable();
	mm_memcpy((void *)frame_addr, kf, sizeof(*kf));
	smap_enable();
	sigframe_fpu_capture(fpu_addr);
}

/* Mask update on handler entry, and SA_RESETHAND. */
static void sigframe_enter_handler(task_t *task, int sig,
				   struct k_sigaction *act)
{
	sigorset_k(&task->signals.blocked, &task->signals.blocked,
		   &act->sa_mask);
	if (!(act->sa_flags & SA_NODEFER))
		sigaddset_k(&task->signals.blocked, sig);
	/* A handler must not be able to make itself unkillable for its
	 * duration, via sa_mask or via being SIGKILL/SIGSTOP itself. */
	sig_strip_unblockable(&task->signals.blocked);

	if (act->sa_flags & SA_RESETHAND)
		act->sa_handler = SIG_DFL;

	task->signals.signal_frame_depth++;
}

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

	sig->sigfd_wq = NULL;

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
/* The signal state a NEW task must not inherit, whatever kind of clone made
 * it.  Both callers below start from a wholesale `mm_memcpy(child, cur,
 * sizeof(task_t))', so every field here is currently a copy of the creator's
 * -- and for the three POINTERS that is not a copy at all, it is a second
 * reference to one object with two owners, each of which frees it on the way
 * out:
 *
 *   - sigfd_wq is the wait queue signalfd readers and sigwait() sleep on.  It
 *     is per-TASK, allocated on first use and freed by sched_destroy_task().
 *     Sharing it meant the first of the two to be reaped freed the other's,
 *     and the survivor's next signal_send() spun forever on a spinlock in
 *     freed memory -- with interrupts disabled, so the machine simply
 *     stopped.
 *   - pending_queue is the list of queued siginfos, freed node by node in
 *     signal_cleanup_task().  Two owners, one list: a double free.
 *   - pending itself: POSIX says a new thread's pending set is empty, and a
 *     copied bit describes a signal whose queued description belongs to
 *     somebody else.
 *
 * The rest is per-task by definition: sigaltstack is explicitly per-thread
 * and not inherited, a saved mask belongs to the sigsuspend that saved it,
 * the interval timers and alarm() are the PROCESS's and are kept by the
 * leader, and a frame depth describes handlers running on this task's stack.
 *
 * What a new task DOES inherit -- the blocked mask and the dispositions -- is
 * left exactly as the memcpy found it, which is what both POSIX fork(2) and
 * pthread_create(3) require. */
static void signal_reset_new_task(task_signal_state_t *csig)
{
	sigemptyset_k(&csig->pending);
	csig->pending_queue = NULL;
	csig->sigfd_wq = NULL;

	sigemptyset_k(&csig->saved_mask);
	csig->in_sigsuspend = 0;

	csig->altstack.ss_sp = NULL;
	csig->altstack.ss_flags = SS_DISABLE;
	csig->altstack.ss_size = 0;

	mm_memset(&csig->itimer_real, 0, sizeof(csig->itimer_real));
	mm_memset(&csig->itimer_virtual, 0, sizeof(csig->itimer_virtual));
	mm_memset(&csig->itimer_prof, 0, sizeof(csig->itimer_prof));
	csig->alarm_ticks = 0;

	csig->signal_frame_depth = 0;
}

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

	signal_reset_new_task(csig);
}

/* The CLONE_THREAD/CLONE_SIGHAND counterpart of signal_fork_copy().
 *
 * A thread that shares its creator's handlers took the other arm of that
 * branch in do_clone(), which set up the shared sighand_struct and then went
 * straight on -- so nothing reset the signal state at all, and the new thread
 * began life holding the creator's queue pointers.  That is the whole of the
 * bug described above; a thread reached it every time, a forked child never
 * did.
 *
 * The dispositions are not copied here: they are the shared sighand_struct's,
 * and the action[] array the memcpy left behind is the same one the creator
 * has.  The blocked mask stays too -- pthread_create(3) says the new thread
 * inherits the creating thread's mask. */
void signal_thread_copy(task_t *child, task_t *parent)
{
	BUG_ON(child == NULL || parent == NULL);
	if (!child || !parent)
		return;
	(void)parent;
	signal_reset_new_task(&child->signals);
}

// Cleanup signal state when task exits
void signal_cleanup_task(task_t *task)
{
	BUG_ON(task == NULL);
	if (!task)
		return;

	task_signal_state_t *sig = &task->signals;

	// Free pending signal queue
	uint64_t flags;
	pending_signal_t *ps;

	/* Unhook the whole list under the lock, then free it outside: a sender
	 * that raced this far is pushing onto the same head. */
	spin_lock_irqsave(&g_sigqueue_lock, &flags);
	ps = sig->pending_queue;
	sig->pending_queue = NULL;
	spin_unlock_irqrestore(&g_sigqueue_lock, flags);

	while (ps) {
		pending_signal_t *next = ps->next;

		kfree(ps);
		ps = next;
	}

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
	pending_signal_t *dead = NULL;
	uint64_t flags;

	for (int s = 1; s < NSIG; s++) {
		if (sigismember_k(mask, s))
			sigdelset_k_atomic(&sig->pending, s);
	}

	spin_lock_irqsave(&g_sigqueue_lock, &flags);
	pp = &sig->pending_queue;
	while (*pp) {
		pending_signal_t *ps = *pp;

		if (sigismember_k(mask, ps->sig)) {
			*pp = ps->next;
			ps->next = dead;
			dead = ps;
			continue;
		}
		pp = &ps->next;
	}
	spin_unlock_irqrestore(&g_sigqueue_lock, flags);

	/* Released only after the lock and the interrupt flag are given back:
	 * kfree() can raise a TLB shootdown IPI. */
	while (dead) {
		pending_signal_t *next = dead->next;

		kfree(dead);
		dead = next;
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

	/* Check if signal is ignored (except SIGKILL/SIGSTOP -- and except
	 * while it is blocked: the disposition that matters is the one in
	 * force when the signal is finally delivered, and the program has
	 * until then to install a handler.  Discarding it here also took it
	 * away from a signalfd watching it, which is a common shape: block
	 * the signal, ignore it, read it from the descriptor.) */
	if (!sig_kernel_only(sig) && !sigismember_k(&sigstate->blocked, sig)) {
		if (act->sa_handler == SIG_IGN) {
			return 0; // Signal ignored
		}
		// Check default action is ignore
		if (act->sa_handler == SIG_DFL &&
		    sig_default_action(sig) == SIG_DFL_IGN) {
			return 0;
		}
	}

	/* Whether this signal was ALREADY pending decides, below, if a second
	 * description of it is worth keeping -- so read it before setting, and
	 * do both in ONE operation.  The task itself may be clearing the bit
	 * for a signal it is delivering on another CPU at this instant; read
	 * and set as separate steps and whichever store lands second wipes the
	 * other out.  Losing the clear is the worse of the two: the signal
	 * stays pending after it was delivered, and its handler runs a second
	 * time for this one send. */
	int was_pending = sigaddset_k_atomic(&sigstate->pending, sig);

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

	/* A BLOCKED signal is one somebody means to READ.  sigwait(),
	 * sigtimedwait() and signalfd() all hand their caller a siginfo, and
	 * what is in it -- who sent it, above all -- exists nowhere else once
	 * this returns.  Dropping it here is what made every signalfd record
	 * read back ssi_pid == 0 for a signal that a perfectly ordinary
	 * kill(2) had sent. */
	if (info && !keep_info && sigismember_k(&sigstate->blocked, sig))
		keep_info = 1;

	/* Non-realtime signals do not queue: one pending instance is all there
	 * ever is, so a second send while the first is still pending adds an
	 * allocation that only the reader of a signal it will never see could
	 * free.  The description already recorded is the one that stands.
	 * (Realtime signals are exempt: queueing every instance is the whole
	 * difference between them and the rest.) */
	if (keep_info && was_pending && sig < SIGRTMIN)
		keep_info = 0;

	if (keep_info) {
		pending_signal_t *ps = alloc_pending_signal();

		if (ps) {
			uint64_t flags;

			/* Filled in before the lock is taken: kalloc() may
			 * sleep, and nothing may sleep holding a spinlock.
			 * Only the link itself has to be serialised. */
			ps->sig = sig;
			mm_memcpy(&ps->info, info, sizeof(siginfo_t));

			spin_lock_irqsave(&g_sigqueue_lock, &flags);
			ps->next = sigstate->pending_queue;
			sigstate->pending_queue = ps;
			spin_unlock_irqrestore(&g_sigqueue_lock, flags);
		}
	}

	/* signalfd readers/pollers of this task sleep on this queue whatever
	 * the mask says. */
	if (sigstate->sigfd_wq)
		poll_notify_wq(sigstate->sigfd_wq);

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
			/* Only a signal that will actually RUN A HANDLER has
			 * an opinion.  SA_RESTART describes what a handler
			 * wants done with the call it interrupted, so a
			 * disposition that runs no handler -- default or
			 * ignore -- expresses nothing, and its zeroed flags
			 * are not a vote against restarting.  Reading them as
			 * one is what made an ignored SIGCHLD arriving during
			 * a wait come back as a spurious EINTR for a program
			 * that had installed no handler at all and could not
			 * have been interrupted by anything it could see. */
			if (sig->action[s].sa_handler == SIG_DFL ||
			    sig->action[s].sa_handler == SIG_IGN) {
				continue;
			}
			// Check if this signal's handler has SA_RESTART
			if (!(sig->action[s].sa_flags & SA_RESTART)) {
				return 0; // At least one signal lacks SA_RESTART
			}
		}
	}

	/* Every handler that will run asked for it -- or none will run, in
	 * which case nothing observable happened and the call simply resumes. */
	return 1;
}

// Dequeue a pending signal (returns signal number, 0 if none)
/* Dequeue a pending signal that IS a member of `want'.
 *
 * The mask signal_dequeue() takes below is a set to SKIP -- a blocked set.
 * A signalfd needs the opposite question asked: it watches a set and wants
 * exactly those.  Handing its watch set to signal_dequeue() answered the
 * inverse, so a descriptor watching SIGUSR1 dequeued everything except
 * SIGUSR1 and reported EAGAIN with the signal sitting pending in front of
 * it -- and the process died of it the moment the mask was restored.
 *
 * SIGKILL and SIGSTOP are never taken here whatever the watch set says:
 * they are not a descriptor's to consume. */
int signal_dequeue_wanted(task_t *task, const kernel_sigset_t *want,
			  siginfo_t *info)
{
	task_signal_state_t *sig;

	if (!task || !want)
		return 0;
	sig = &task->signals;

	for (int s = 1; s < NSIG; s++) {
		if (sig_kernel_only(s))
			continue;
		if (!sigismember_k(want, s) || !sigismember_k(&sig->pending, s))
			continue;

		/* Claim it.  The clear reports whether the bit was still set,
		 * so the test above and the take here cannot both succeed for
		 * one send when a sender is touching the same word. */
		if (!sigdelset_k_atomic(&sig->pending, s))
			continue;
		if (info) {
			pending_signal_t *ps = NULL;
			pending_signal_t **pp;
			uint64_t flags;

			mm_memset(info, 0, sizeof(*info));
			info->si_signo = s;
			info->si_code = SI_USER;

			spin_lock_irqsave(&g_sigqueue_lock, &flags);
			pp = &sig->pending_queue;
			while (*pp) {
				if ((*pp)->sig == s) {
					ps = *pp;
					*pp = ps->next;
					break;
				}
				pp = &(*pp)->next;
			}
			spin_unlock_irqrestore(&g_sigqueue_lock, flags);

			if (ps) {
				mm_memcpy(info, &ps->info, sizeof(siginfo_t));
				kfree(ps);
			}
		}
		return s;
	}
	return 0;
}

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

			/* Remove from pending, and take it only if the bit
			 * was still ours to take: a sender on another CPU
			 * shares this word. */
			if (!sigdelset_k_atomic(&sig->pending, signum))
				continue;

			// Find and remove from queue if present
			if (info) {
				pending_signal_t *ps = NULL;
				pending_signal_t **pp;
				uint64_t flags;

				mm_memset(info, 0, sizeof(*info));
				info->si_signo = signum;
				info->si_code = SI_USER;

				spin_lock_irqsave(&g_sigqueue_lock, &flags);
				pp = &sig->pending_queue;
				while (*pp) {
					if ((*pp)->sig == signum) {
						ps = *pp;
						*pp = ps->next;
						break;
					}
					pp = &(*pp)->next;
				}
				spin_unlock_irqrestore(&g_sigqueue_lock, flags);

				if (ps) {
					mm_memcpy(info, &ps->info,
						  sizeof(siginfo_t));
					kfree(ps);
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
/* (SIGFRAME_REDZONE itself is defined at the top of the file, ahead of the
 * placement helper that uses it.) */

// Setup a signal frame on the user stack (syscall-return path).
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

	uint64_t frame_addr, fpu_addr;
	int on_alt;

	if (sigframe_place(task, act, user_rsp, &frame_addr, &fpu_addr,
			   &on_alt) < 0)
		return -1;

	// Build the signal frame in kernel memory first
	signal_frame_t kframe;
	mm_memset(&kframe, 0, sizeof(kframe));

	/* The interrupted context, as the handler will see it in
	 * uc_mcontext.  A syscall may clobber the caller-saved registers
	 * (the ABI allows it), so only the callee-saved set and RAX -- the
	 * syscall's return value -- are meaningful here; the rest read 0. */
	uint64_t *g = kframe.uc.uc_mcontext.gregs;
	g[REG_RIP] = user_rip;
	g[REG_RSP] = user_rsp;
	g[REG_EFL] = user_rflags;
	g[REG_RBP] = task->syscall_rbp;
	g[REG_RBX] = task->syscall_rbx;
	g[REG_R12] = task->syscall_r12;
	g[REG_R13] = task->syscall_r13;
	g[REG_R14] = task->syscall_r14;
	g[REG_R15] = task->syscall_r15;
	g[REG_RAX] = task->syscall_rax;
	g[REG_CSGSFS] = 0x33; /* user code selector */

	sigframe_fill(&kframe, task, sig, info, act, frame_addr, fpu_addr,
		      on_alt);
	sigframe_commit(&kframe, frame_addr, fpu_addr);
	sigframe_enter_handler(task, sig, act);

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

	/* Handler arguments 2 and 3.  An SA_SIGINFO handler is entered as
	 * handler(sig, siginfo_t *, void *ucontext) and must be able to
	 * dereference both: Xorg's crash handler reads si_code before
	 * anything else, and a language runtime's fault handler resumes the
	 * faulting instruction by editing the ucontext.  Both live in the
	 * frame just written.  syscall.asm loads these two slots into
	 * RSI/RDX on the handler-call path. */
	cpu->syscall_saved_user_rsi =
		(act->sa_flags & SA_SIGINFO) ?
			frame_addr + __builtin_offsetof(signal_frame_t, info) :
			0;
	cpu->syscall_saved_user_rdx =
		frame_addr + __builtin_offsetof(signal_frame_t, uc);

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

	uint64_t frame_addr, fpu_addr;
	int on_alt;

	if (sigframe_place(task, act, user_rsp, &frame_addr, &fpu_addr,
			   &on_alt) < 0)
		return -1;

	// Build the signal frame in kernel memory first
	signal_frame_t kframe;
	mm_memset(&kframe, 0, sizeof(kframe));

	/* The whole register file: an interrupt can land on any instruction,
	 * with every register live. */
	uint64_t *g = kframe.uc.uc_mcontext.gregs;
	g[REG_RIP] = user_rip;
	g[REG_RSP] = user_rsp;
	g[REG_EFL] = user_rflags;
	g[REG_RBP] = frame->rbp;
	g[REG_RBX] = frame->rbx;
	g[REG_R12] = frame->r12;
	g[REG_R13] = frame->r13;
	g[REG_R14] = frame->r14;
	g[REG_R15] = frame->r15;
	g[REG_RCX] = frame->rcx;
	g[REG_RDX] = frame->rdx;
	g[REG_RSI] = frame->rsi;
	g[REG_RDI] = frame->rdi;
	g[REG_R8] = frame->r8;
	g[REG_R9] = frame->r9;
	g[REG_R10] = frame->r10;
	g[REG_R11] = frame->r11;
	g[REG_RAX] = frame->rax;
	g[REG_CSGSFS] = frame->cs & 0xFFFF;
	g[REG_TRAPNO] = frame->int_no;
	g[REG_ERR] = frame->err_code;
	if (info && (sig == SIGSEGV || sig == SIGBUS))
		g[REG_CR2] = (uint64_t)info->si_addr;

	sigframe_fill(&kframe, task, sig, info, act, frame_addr, fpu_addr,
		      on_alt);
	sigframe_commit(&kframe, frame_addr, fpu_addr);
	sigframe_enter_handler(task, sig, act);

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
	/* Second and third: the siginfo in the frame (SA_SIGINFO only) and
	 * the ucontext. */
	frame->rsi = (act->sa_flags & SA_SIGINFO) ?
			     frame_addr + __builtin_offsetof(signal_frame_t, info) :
			     0;
	frame->rdx = frame_addr + __builtin_offsetof(signal_frame_t, uc);
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

	/* Put the interrupted computation's extended register state back
	 * into the hardware.  If the task is preempted between here and the
	 * return to user mode, the context switch saves exactly this state
	 * into the task's own area and restores it again -- so the resumed
	 * code sees its registers regardless. */
	sigframe_fpu_restore(task, sigframe_fpu_addr(frame_addr));

	/* The registers come out of uc_mcontext -- possibly edited by the
	 * handler, which is the point of handing it a ucontext. */
	const uint64_t *g = kframe.uc.uc_mcontext.gregs;

	// Update task's saved values first (safe without cli)
	task->syscall_rip = g[REG_RIP];
	task->syscall_rsp = g[REG_RSP];
	/* kframe came off the user's own stack -- see user_rflags_sanitize(). */
	task->syscall_rflags = user_rflags_sanitize(g[REG_EFL]);
	task->syscall_rbp = g[REG_RBP];
	task->syscall_rbx = g[REG_RBX];
	task->syscall_r12 = g[REG_R12];
	task->syscall_r13 = g[REG_R13];
	task->syscall_r14 = g[REG_R14];
	task->syscall_r15 = g[REG_R15];

	// Save RAX (syscall return value) for assembly to restore
	task->syscall_rax = g[REG_RAX];

	/* Restore signal mask.  kframe was just read back from the user's own
	 * stack, so the mask is user-writable: a process could otherwise edit
	 * it to include SIGKILL and sigreturn itself unkillable. */
	task->signals.blocked = kframe.uc.uc_sigmask;
	sig_strip_unblockable(&task->signals.blocked);

	// Clear sigsuspend flag if set
	task->signals.in_sigsuspend = 0;

	// CRITICAL: Disable interrupts before modifying per-CPU syscall return context
	// This prevents a race where a timer interrupt could cause a context switch
	__asm__ volatile("cli" ::: "memory");

	// Restore registers to per-CPU storage (output to syscall.asm)
	percpu_t *cpu = this_cpu();
	cpu->syscall_saved_user_rip = g[REG_RIP];
	cpu->syscall_user_rsp = g[REG_RSP];
	cpu->syscall_saved_user_rflags = user_rflags_sanitize(g[REG_EFL]);
	cpu->syscall_saved_user_rbp = g[REG_RBP];
	cpu->syscall_saved_user_rbx = g[REG_RBX];
	cpu->syscall_saved_user_r12 = g[REG_R12];
	cpu->syscall_saved_user_r13 = g[REG_R13];
	cpu->syscall_saved_user_r14 = g[REG_R14];
	cpu->syscall_saved_user_r15 = g[REG_R15];
	cpu->syscall_saved_user_rax = g[REG_RAX];

	/* And the rest of the register file.
	 *
	 * A syscall return can leave these clobbered -- the ABI allows it -- but
	 * a signal return cannot: the signal may have interrupted user code at
	 * any instruction, with every register live.  They used to be dropped
	 * (and the assembly then zeroed them), so a signal arriving between a
	 * register load and its use corrupted the interrupted program at random.
	 * That is not a rare window: it is every instruction that is not a
	 * syscall. */
	cpu->syscall_saved_user_rdi = g[REG_RDI];
	cpu->syscall_saved_user_rsi = g[REG_RSI];
	cpu->syscall_saved_user_rdx = g[REG_RDX];
	cpu->syscall_saved_user_rcx = g[REG_RCX];
	cpu->syscall_saved_user_r8 = g[REG_R8];
	cpu->syscall_saved_user_r9 = g[REG_R9];
	cpu->syscall_saved_user_r10 = g[REG_R10];
	cpu->syscall_saved_user_r11 = g[REG_R11];

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

static void kill_task(task_t *t, int sig, task_t *sender)
{
	if (!t) {
		return;
	}
	// Use sched_signal_task which properly handles SIGKILL/SIGSTOP
	// and other signals with their default actions.  The sender travels
	// with it: kill(2) is required to name itself in the siginfo, and a
	// sigwait()/signalfd() reader has no other way to learn who signalled.
	sched_signal_task_from(t, sig, sender);
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
	kill_task(t, (int)sig, self);
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

/* SYS_RT_SIGTIMEDWAIT -- accept one of `set', waiting for it if need be.
 *
 * sigwait(3) and sigwaitinfo(3) land here.  Their whole premise is that the
 * caller has BLOCKED the signals it names: they are not to be delivered to a
 * handler, they are to be handed to this call.  Three things followed from
 * that premise and all three were wrong here:
 *
 *   - The dequeue took the WRONG SET.  signal_dequeue()'s mask is a set to
 *     SKIP, so passing the wanted set told it to consume everything EXCEPT
 *     what the caller asked for.  The awaited signal was reported to the
 *     caller and left pending, so the moment the caller restored its mask --
 *     the very next line of any sigwait() user -- the signal it had just
 *     "received" was delivered for real and killed it.  signal_dequeue_wanted()
 *     is the one that asks the question this call means.
 *
 *   - The wait could not be woken.  A bare `state = TASK_BLOCKED; schedule()'
 *     with no queue and no timer registers no wakeup with anything, and the
 *     one sweep that rescues blocked tasks only looks at UNBLOCKED pending
 *     signals -- which, by the premise above, this is never waiting for.  The
 *     signalfd queue is exactly the right thing to sleep on: signal_send()
 *     notifies it for every signal queued to the task, blocked or not.
 *
 *   - Nothing could interrupt it, SIGKILL included.  The loop checked only
 *     has_exited, so a caller waiting for a signal that never came never
 *     returned to user space and so never reached the place where a fatal
 *     signal is acted on.  That is an unkillable process holding its terminal,
 *     which from the console is indistinguishable from a hung machine.
 *
 * The timeout was decorative for the same reason: the deadline was computed
 * but never armed, so it was only ever consulted after a wake that could not
 * happen.  It is armed on the task here, which is what the timer sweep looks
 * at.
 */
static int sigtimedwait_ready(task_t *t, const kernel_sigset_t *want)
{
	for (int s = 1; s < NSIG; s++) {
		if (sig_kernel_only(s))
			continue;
		if (sigismember_k(want, s) &&
		    sigismember_k(&t->signals.pending, s))
			return 1;
	}
	return 0;
}

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
	/* Not waitable by anybody: these two are acted on, never handed over. */
	sigdelset_k(&wait_set, SIGKILL);
	sigdelset_k(&wait_set, SIGSTOP);

	int timed = 0;
	uint64_t deadline = 0;
	if (timeout_ptr) {
		struct k_timespec timeout;

		if (copy_from_user(&timeout, (void *)timeout_ptr,
				   sizeof(struct k_timespec)) != 0) {
			return -EFAULT;
		}
		if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 ||
		    timeout.tv_nsec >= 1000000000L) {
			return -EINVAL;
		}
		uint32_t freq = timer_get_frequency();
		uint64_t ticks =
			(uint64_t)timeout.tv_sec * freq +
			(uint64_t)timeout.tv_nsec * freq / 1000000000ULL;
		/* A real timeout must never round down to "poll": a 1 ms wait
		 * on a 100 Hz tick is short, not zero. */
		if (ticks == 0 && (timeout.tv_sec || timeout.tv_nsec))
			ticks = 1;
		timed = 1;
		deadline = timer_ticks() + ticks;
	}

	for (;;) {
		siginfo_t info;
		int sig;

		mm_memset(&info, 0, sizeof(info));
		sig = signal_dequeue_wanted(cur, &wait_set, &info);
		if (sig > 0) {
			if (info_ptr &&
			    copy_to_user((void *)info_ptr, &info,
					 sizeof(siginfo_t)) != 0) {
				return -EFAULT;
			}
			return sig;
		}

		/* A signal the caller did NOT ask for and has not blocked has
		 * to be delivered instead -- that is what ends this wait
		 * early, and it is how a fatal one gets acted on at all. */
		if (signal_pending(cur))
			return -EINTR;
		if (cur->has_exited)
			return -EINTR;
		if (timed && timer_ticks() >= deadline)
			return -EAGAIN;

		struct wait_queue_head *wq = signalfd_task_wq(cur);
		if (!wq)
			return -ENOMEM;

		struct wait_queue_entry we;
		uint64_t fl = local_irq_save();

		wq_entry_init(&we, cur);
		wq_add(wq, &we);
		/* Re-checked with the entry already on the queue and
		 * interrupts off: signal_send() sets the pending bit before it
		 * notifies, so a signal that arrives in the window is either
		 * seen here or wakes us from the queue. */
		if (!sigtimedwait_ready(cur, &wait_set) && !signal_pending(cur) &&
		    !(timed && timer_ticks() >= deadline)) {
			cur->wait_channel = wq;
			cur->wakeup_tick = timed ? deadline : 0;
			cur->state = TASK_BLOCKED;
			local_irq_restore(fl);
			sched_schedule();
		} else {
			local_irq_restore(fl);
		}
		wq_remove(wq, &we);
		/* A queue wake does not disarm the timer; leaving it set would
		 * cut short whatever this task blocks on next. */
		cur->wakeup_tick = 0;
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

/* Park until a signal the task has NOT blocked is pending.
 *
 * pause(2) and sigsuspend(2) are both exactly this wait, and both used to
 * write it as `state = TASK_BLOCKED' once, outside a loop that called
 * sched_schedule() round it.  Two things were wrong with that shape:
 *
 *   - after the first wake the state was no longer BLOCKED, so every further
 *     turn of the loop was a plain yield: a spin at full CPU for as long as
 *     the wait lasted;
 *   - when a signal was ALREADY pending on entry the loop body never ran, so
 *     the caller went back to user space still marked TASK_BLOCKED -- a task
 *     the scheduler is entitled to leave off its run queue at the next
 *     preemption, which is a process that simply stops for good.
 *
 * The queue slept on is the one signal_send() notifies for every signal
 * queued to the task, so the wake is prompt rather than waiting on the timer
 * sweep; the re-check happens with the entry already queued and interrupts
 * off, so a signal arriving in the window cannot be missed.
 */
static void signal_wait_pending(task_t *cur)
{
	for (;;) {
		if (signal_pending(cur) || cur->has_exited)
			break;

		struct wait_queue_head *wq = signalfd_task_wq(cur);
		struct wait_queue_entry we;
		uint64_t fl;

		if (wq) {
			wq_entry_init(&we, cur);
			fl = local_irq_save();
			wq_add(wq, &we);
		} else {
			/* No memory for a queue: the timer sweep still wakes a
			 * blocked task with an unblocked signal pending, which
			 * is precisely the condition waited for here. */
			fl = local_irq_save();
		}
		if (!signal_pending(cur)) {
			cur->wait_channel = wq;
			cur->state = TASK_BLOCKED;
			local_irq_restore(fl);
			sched_schedule();
		} else {
			local_irq_restore(fl);
		}
		if (wq)
			wq_remove(wq, &we);
	}
	cur->state = TASK_RUNNING;
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

	/* Install the temporary mask.  The restore is DEFERRED, through the
	 * same machinery ppoll()/pselect() use (poll_sigmask_install and the
	 * consumer at the syscall exit): the caller's mask must stay OFF
	 * until the signal that ended the wait has had its handler set up,
	 * and the handler's frame must carry the CALLER's mask so sigreturn
	 * puts that back once the handler is done.  That is the contract:
	 * the handler runs under the temporary mask, and sigsuspend()
	 * returns after it, with the original mask restored.
	 *
	 * Restoring the mask right here, before returning -- what this used
	 * to do -- broke both halves at once.  The signal that ended the
	 * wait was still only PENDING; with the caller's mask back on, a
	 * caller who had it blocked (the reason to sigsuspend at all) got no
	 * handler run and returned with the signal still queued -- to fire,
	 * spuriously, whenever that mask was next lifted.
	 *
	 * That killed web processes.  The script engine parks a thread by
	 * signalling it; the handler publishes the thread's registers and
	 * sigsuspends awaiting the SAME signal as the resume.  With the
	 * eager restore, the resume signal survived the sigsuspend and
	 * re-entered the handler after it had already finished -- which
	 * looks like a fresh park request, so the thread published its
	 * registers, posted the acknowledgement semaphore ONE EXTRA TIME,
	 * and froze with nobody intending to wake it.  Every suspension
	 * after that consumed a stale acknowledgement and proceeded while
	 * the target was NOT parked: the collector walked the stacks of
	 * running threads (memory corruption with no pattern), and the
	 * register pointer it read could be nulled mid-read by the escaping
	 * thread -- the reproducible tip of it, a crash in getRegisters()
	 * loading address zero. */
	cur->sigmask_saved = cur->signals.blocked;
	cur->sigmask_restore_pending = 1;
	cur->signals.blocked = newmask;
	cur->signals.in_sigsuspend = 1;

	// Can't block SIGKILL/SIGSTOP
	sig_strip_unblockable(&cur->signals.blocked);

	// Wait for a signal the temporary mask lets through
	signal_wait_pending(cur);

	cur->signals.in_sigsuspend = 0;
	/* No mask restore here.  On the way out, signal delivery runs first
	 * -- still under the temporary mask, so the awaited handler actually
	 * fires, taking the parked mask into its frame (sigframe_fill) for
	 * sigreturn to restore -- and poll_sigmask_restore_pending() puts
	 * the mask back only if no handler claimed it (the signal was
	 * fetched by another thread, or its action terminates us anyway). */
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
	signal_wait_pending(cur);

	return -EINTR; // pause always returns EINTR
}

