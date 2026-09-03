// LikeOS-64 -- vmwgfx: command buffers, MOBs and object tables.
#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/waitq.h>
#include <kernel/hal/lapic.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ---- command buffers ---------------------------------------------------- */
//
// ASYNCHRONOUS, like the reference implementation: a submission hands the
// device a buffer and returns; completion and errors are collected later
// (on the next claim, from the fence tick, or by a synchronous waiter).
// The old shape -- one buffer, and every submission waiting for the device
// to finish it -- was correct and unusable: a compositor issues hundreds of
// submissions per second, and each one blocked its caller for the host's
// full execution of the batch.  The desktop ran at the speed of that wait.
//
// The device processes the buffers of one command-buffer context strictly
// in submission order, which is what makes the rest of this file simple:
// everything goes to context 0, so a synchronous waiter that sees its own
// buffer complete knows every earlier submission is complete too, and a
// fence submitted through this channel signals only after the batches
// before it.

#define VMW_CB_SLOTS 16
#define VMW_CB_SLOT_BYTES (128 * 1024)
/* How much is gathered before it has to go.  Comfortably inside a slot, and
 * far more than a frame's worth of object commands. */
#define VMW_PEND_BYTES (64 * 1024)

/* Slot states.
 *
 * CB_FILLING is not a formality.  A slot becomes reapable only once it has
 * actually been handed to the device: between claiming it and ringing the
 * doorbell its header still holds the PREVIOUS submission's status, and a
 * slot published as in-flight before that is read by any concurrent reaper
 * as already COMPLETED.  The reaper then frees it, another submitter claims
 * it, and two threads fill the same buffer while the first one goes on to
 * ring a header describing the other's bytes -- arbitrary command streams
 * reaching the device, which is corruption anywhere on the screen rather
 * than in one surface. */
#define CB_FREE 0
#define CB_FILLING 1 /* claimed, not yet given to the device: NOT reapable */
#define CB_ASYNC 2   /* in flight; anyone may reap it */
#define CB_SYNC 3    /* in flight; the submitter waits and reaps it */
#define CB_REAPING 4 /* being processed */

/* Defined below, with the other command-buffer primitives it is built on. */
static int cb_start_context(struct vmw_device *v, uint32_t ctx);
static int cb_preempt_context(struct vmw_device *v, uint32_t ctx);
static int pend_add(struct vmw_device *v, const uint8_t *p, uint32_t bytes,
		    uint32_t dx_cid);
static void cb_slot_completed(struct vmw_device *v, struct vmw_cb_slot *s);
static void pend_lock(struct vmw_device *v);
static void pend_unlock(struct vmw_device *v);
static int pend_flush_locked(struct vmw_device *v);
static void defer_drain(struct vmw_device *v);
void vmw_cmd_drain(struct vmw_device *v);
static int cb_slot_handle_error(struct vmw_device *v, struct vmw_cb_slot *s,
				int timed_out);

static int vmw_cmdbuf_bringup(struct vmw_device *v)
{
	v->cb_busy = 0;
	v->cb_nslots = 0;
	v->cb_last_progress_us = timer_get_precise_us();
	wq_head_init(&v->cb_wq, "vmw_cb");
	if (!v->has_cmdbuf)
		return 0;
	for (int i = 0; i < VMW_CB_SLOTS; i++) {
		struct vmw_cb_slot *s = &v->cb_slot[i];
		s->hdr_phys = mm_allocate_physical_page();
		if (!s->hdr_phys)
			break;
		s->hdr = phys_to_virt(s->hdr_phys);
		mm_memset((void *)s->hdr, 0, PAGE_SIZE);
		s->buf_phys = mm_allocate_contiguous_pages(VMW_CB_SLOT_BYTES / PAGE_SIZE);
		if (!s->buf_phys) {
			mm_free_physical_page(s->hdr_phys);
			s->hdr = NULL;
			s->hdr_phys = 0;
			break;
		}
		s->buf = phys_to_virt(s->buf_phys);
		s->state = CB_FREE;
		v->cb_nslots = i + 1;
	}
	if (v->cb_nslots < 2) {
		kprintf("[drm] vmwgfx: no memory for command buffers; using the FIFO\n");
		goto fail;
	}
	v->cb_size = VMW_CB_SLOT_BYTES;
	v->pend = kalloc(VMW_PEND_BYTES);
	if (!v->pend) {
		kprintf("[drm] vmwgfx: no memory for the command batch; using the FIFO\n");
		goto fail;
	}
	v->pend_len = 0;
	v->pend_dx = SVGA3D_INVALID_ID;

	/* Command-buffer context 0 has to be STARTED before it will run
	 * anything.  A context is stopped until the guest starts it, and a
	 * buffer submitted to a stopped context is simply never looked at:
	 * its status stays SVGA_CB_STATUS_NONE and the only thing that ends
	 * the wait is the submission timeout.
	 *
	 * Nothing sent this before, so every command-buffer submission
	 * failed on a device that was working perfectly -- the first one
	 * being SET_OTABLE_BASE64, which is why the guest-backed path could
	 * never come up.
	 *
	 * Falling back rather than failing: with command buffers off, the
	 * FIFO arm of vmw_cmd_submit() carries the same commands.  DX goes
	 * with them, because a DX context can only be addressed through a
	 * command buffer. */
	if (cb_start_context(v, SVGA_CB_CONTEXT_0) != 0) {
		kprintf("[drm] vmwgfx: command-buffer context would not start; "
			"using the FIFO\n");
		goto fail;
	}
	v->cb_ready = 1;
	/* From here the FIFO is not this driver's command stream, and this
	 * device's one fence register belongs to the channel above.  The
	 * layer underneath stops emitting fences of its own; see
	 * vmsvga2_set_cmdbuf_owner(). */
	vmsvga2_set_cmdbuf_owner(1);
	return 0;
fail:
	for (int i = 0; i < v->cb_nslots; i++) {
		struct vmw_cb_slot *s = &v->cb_slot[i];
		mm_free_contiguous_pages(s->buf_phys, VMW_CB_SLOT_BYTES / PAGE_SIZE);
		mm_free_physical_page(s->hdr_phys);
		s->hdr = NULL;
		s->buf = NULL;
	}
	v->cb_nslots = 0;
	v->cb_size = 0;
	v->has_cmdbuf = 0;
	v->has_dx = 0;
	vmsvga2_set_cmdbuf_owner(0); /* the FIFO carries everything again */
	return 0;
}

/* Map a final device status to an errno. */
static int cb_status_rc(uint32_t st)
{
	switch (st) {
	case SVGA_CB_STATUS_COMPLETED:
		return 0;
	case SVGA_CB_STATUS_QUEUE_FULL:
		return -EBUSY;
	case SVGA_CB_STATUS_COMMAND_ERROR:
		return -EINVAL;
	default:
		return -EIO;
	}
}

/* The short claim around programming the doorbell registers: two register
 * writes, nothing held across any wait. */
static void cb_channel_acquire(struct vmw_device *v)
{
	while (__atomic_test_and_set(&v->cb_busy, __ATOMIC_ACQUIRE)) {
		if (sched_current() && irqs_enabled())
			sched_yield_in_kernel();
		else
			__asm__ volatile("pause");
	}
}

static void cb_channel_release(struct vmw_device *v)
{
	__atomic_clear(&v->cb_busy, __ATOMIC_RELEASE);
}

/* Hand the device slot `s' (payload already in s->buf, described by the
 * bookkeeping fields).  Writing COMMAND_LOW is what starts it;
 * COMMAND_HIGH must be written first. */
static void cb_slot_ring(struct vmw_device *v, struct vmw_cb_slot *s)
{
	volatile SVGACBHeader *h = s->hdr;

	mm_memset((void *)h, 0, sizeof(*h));
	h->status = SVGA_CB_STATUS_NONE;
	h->flags = s->flags;
	h->length = s->bytes;
	h->ptr.pa = s->buf_phys;
	h->dxContext = s->dx;
	__asm__ volatile("" ::: "memory");
	s->submitted_us = timer_get_precise_us();
	uint64_t pa = s->hdr_phys | s->ctx;
	cb_channel_acquire(v);
	/* The ticket is taken inside the claim, so ticket order IS the order
	 * the device was told about the buffers.  Repairing the channel has
	 * to hand them back in that order: a define that follows the command
	 * using it is simply a second error. */
	s->seq = ++v->cb_next_seq;
	vmsvga2_hw_write_reg(SVGA_REG_COMMAND_HIGH, (uint32_t)(pa >> 32));
	vmsvga2_hw_write_reg(SVGA_REG_COMMAND_LOW, (uint32_t)pa);
	cb_channel_release(v);
}

#define VMW_CB_TIMEOUT_US 2000000ULL

/* Has a wait on the device run out of patience?
 *
 * The deadline runs from the LATER of two moments: when this buffer was
 * handed to the device, and the last time the device finished anything at
 * all.  Both halves are needed, and using either alone is wrong.
 *
 * Without the submission time, a wait that BEGINS after an idle spell is
 * already past its deadline before the device has been asked for anything:
 * `cb_last_progress_us' is device-wide and stops advancing whenever the
 * queue is empty, which it is for as long as userspace is busy doing
 * anything else.  The first submission after such a gap was then declared
 * timed out microseconds after being made -- the device never got a chance
 * to answer.  For an asynchronous caller that only cost a spurious context
 * restart; for a synchronous one it is a hard -ETIMEDOUT handed back to
 * userspace for a device that is perfectly healthy.
 *
 * Without the device-wide time, a slot queued behind heavy work would be
 * declared dead on schedule while the device chews through the batches in
 * front of it -- measuring queue latency and calling a busy device broken,
 * with a context restart as the "repair" that throws the real work away.
 * That is what the note below is about, and it still holds.
 *
 * `submitted_us' has been recorded by cb_slot_ring() all along; nothing
 * read it. */
static int cb_deadline_passed(struct vmw_device *v, uint64_t started_us)
{
	uint64_t base = started_us > v->cb_last_progress_us ?
				started_us :
				v->cb_last_progress_us;

	return timer_get_precise_us() - base > VMW_CB_TIMEOUT_US;
}

/* Collect finished asynchronous slots: free the completed, and -- where the
 * caller is allowed to -- name and repair the failed.
 *
 * `may_recover' is not a preference.  Repairing a rejected buffer restarts
 * the device's context, which is itself a submission: it claims a slot,
 * waits for the device, and yields while it does.  That is fine on the
 * thread that came here to submit something, and fatal from an interrupt
 * handler -- yielding there resumes somebody else on a stack that was
 * never meant to be left, and the return from the interrupt faults on its
 * own IRET.  Naming the failure is out too: the batch dump is many lines
 * of console output, and freeing deferred pages takes the allocator's
 * lock.
 *
 * So the interrupt only collects what has plainly finished, and anything
 * that needs doing about a failure waits for the next submitter -- which
 * is where the reference driver puts it as well, in a work item rather
 * than in the interrupt. */
static void cb_reap(struct vmw_device *v, int may_recover)
{
	for (int i = 0; i < v->cb_nslots; i++) {
		struct vmw_cb_slot *s = &v->cb_slot[i];
		int st = CB_ASYNC;

		if (s->state != CB_ASYNC)
			continue;
		volatile SVGACBHeader *h = s->hdr;
		/* A buffer that has not started is not a buffer that is
		 * stuck.  With submissions in flight the device works
		 * through one context in order, so a slot waiting its turn
		 * behind heavy work can sit unstarted for as long as that
		 * work takes -- timing it from ITS submission measures queue
		 * latency and calls a busy device a broken one.  Worse, the
		 * recovery is a context restart, which throws away the
		 * batches actually being executed.
		 *
		 * So the watchdog is device-wide: something is wrong only if
		 * NOTHING has completed for the timeout, which is true of a
		 * wedged device and false of a loaded one. */
		int timed_out = may_recover &&
				h->status == SVGA_CB_STATUS_NONE &&
				cb_deadline_passed(v, s->submitted_us);
		if (h->status == SVGA_CB_STATUS_NONE && !timed_out)
			continue;
		/* Finished, but not well: leave it for a caller that is
		 * allowed to do something about it. */
		if (!may_recover && h->status != SVGA_CB_STATUS_COMPLETED)
			continue;
		if (!__atomic_compare_exchange_n(&s->state, &st, CB_REAPING, 0,
						 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			continue;
		/* Re-check under ownership. */
		if (h->status == SVGA_CB_STATUS_NONE && !timed_out) {
			__atomic_store_n(&s->state, CB_ASYNC, __ATOMIC_RELEASE);
			continue;
		}
		v->cb_last_progress_us = timer_get_precise_us();
		if (h->status == SVGA_CB_STATUS_COMPLETED)
			cb_slot_completed(v, s);
		else if (cb_slot_handle_error(v, s, timed_out)) {
			/* Handed back to the device -- the repaired payload,
			 * or the same one again: the slot is in flight and
			 * must not be reused. */
			__atomic_store_n(&s->state, CB_ASYNC, __ATOMIC_RELEASE);
			continue;
		}
		/* Also for a buffer given up on: the work is gone either way,
		 * and a client left waiting for a fence that can no longer
		 * arrive would sit out its whole timeout for nothing. */
		cb_slot_completed(v, s);
		__atomic_store_n(&s->state, CB_FREE, __ATOMIC_RELEASE);
		/* A slot is free: whoever is asleep for one may have it.  From
		 * the interrupt as well as from a submitter -- the wait queue
		 * is the same one the fence layer wakes from here. */
		wq_wake_all(&v->cb_wq);
	}
}

/* A buffer the device finished.  If it carried a fence, that fence -- and
 * every one before it, since one context executes in submission order --
 * has now passed, and this is the moment to say so.
 *
 * Nothing else can say it.  The device's fence register is the FIFO's: the
 * console writes SVGA_CMD_FENCE into the FIFO out of the SAME counter these
 * numbers come from, the FIFO drains on its own schedule, and the register
 * therefore runs ahead of whatever a command-buffer context has actually
 * executed.  Reading it as "fences up to here have passed" made every wait
 * on a command-buffer fence return at once: a client's readback then mapped
 * its surface before the device had written a pixel into it and got the
 * frame before -- rendering that had plainly worked, arriving one frame
 * late for ever.  (Seen as: a GL draw landing on the host -- the kernel's
 * own readback showed it -- while glReadPixels kept returning the clear
 * colour.) */
static void cb_slot_completed(struct vmw_device *v, struct vmw_cb_slot *s)
{
	if (!s->fence_seq)
		return;
	uint32_t seq = s->fence_seq;

	s->fence_seq = 0;
	drm_fence_signal_upto(&v->drm, seq);
}

/* ---- waiting without paying a timeslice for it --------------------------
 *
 * Two places on the submission path wait for something that is normally
 * over in microseconds: the gather lock, held for a copy and two register
 * writes, and a free command-buffer slot, which the device hands back a
 * few hundred microseconds after it was rung.  Both used to wait by
 * yielding the processor, and a yield is not a short wait: the task goes
 * to the back of its run queue and whatever is in front of it runs for up
 * to its whole timeslice -- SCHED_TIME_SLICE ticks of the periodic timer,
 * twenty milliseconds.  With a display server and a browser submitting
 * against each other, a lock held for five microseconds cost the loser a
 * frame, and it showed up in the frame accounting as `slotwait' events of
 * seven to fourteen milliseconds and presents of two to four.
 *
 * So a short wait spins -- with `pause', bounded by the timestamp counter
 * -- and only a long one gives the processor up.  A waiter for a slot then
 * SLEEPS rather than yields, woken by the reaper the moment a slot comes
 * free, with a timer behind it for the host without a completion
 * interrupt.  The same shape as the fence wait in drm_fence.c, for the
 * same reason. */
#define VMW_SPIN_LOCK_US 100  /* the gather lock: a copy and a doorbell */
#define VMW_SPIN_SLOT_US 200  /* a slot: the device's usual turnaround */
#define VMW_SLOT_POLL_NS 1000000ULL /* asleep for a slot: look again after */

static uint64_t spin_ticks(uint64_t us)
{
	return lapic_get_tsc_freq() / 1000000ULL * us;
}

static int can_sleep(void)
{
	return sched_current() && irqs_enabled();
}

static int cb_any_slot_free(const struct vmw_device *v)
{
	for (int i = 1; i < v->cb_nslots; i++)
		if (v->cb_slot[i].state == CB_FREE)
			return 1;
	return 0;
}

/* The poll deadline for a sleeping slot waiter.  From the timer, so it
 * claims the task the way every waker must: a claim without an enqueue
 * strands it. */
static void cb_slot_poll_wake(hrtimer_t *t)
{
	task_t *task = t->arg;

	if (sched_claim_wake(task, TASK_BLOCKED)) {
		task->wait_channel = NULL;
		sched_enqueue_ready(task);
	}
}

/* Sleep until the reaper frees a slot, or the poll interval passes. */
static void cb_slot_sleep(struct vmw_device *v)
{
	task_t *cur = sched_current();
	struct wait_queue_entry we;
	hrtimer_t poll_timer;
	int highres = hrtimer_is_highres();
	uint64_t fl;

	if (highres)
		hrtimer_init(&poll_timer, cb_slot_poll_wake, cur);
	fl = local_irq_save();
	wq_entry_init(&we, cur);
	wq_add(&v->cb_wq, &we);
	/* Queued first, then looked again: a slot freed between the failed
	 * pass and the add would otherwise be a wake nobody was there for. */
	if (cb_any_slot_free(v)) {
		local_irq_restore(fl);
		wq_remove(&v->cb_wq, &we);
		return;
	}
	cur->wait_channel = &v->cb_wq;
	if (highres)
		hrtimer_start(&poll_timer, hrtimer_now_ns() + VMW_SLOT_POLL_NS);
	else
		cur->wakeup_tick = timer_ticks() + 1;
	cur->state = TASK_BLOCKED;
	local_irq_restore(fl);
	sched_schedule();
	if (highres)
		hrtimer_cancel(&poll_timer);
	/* Disarm: this deadline belongs to this wait and to nothing after
	 * it -- see the note above sched_claim_wake(). */
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	wq_remove(&v->cb_wq, &we);
}

/* Claim a free slot, reaping while none is available.  The slot comes back
 * in CB_FILLING: the caller fills it, rings it, and only then publishes it
 * with cb_slot_published(). */
static struct vmw_cb_slot *cb_slot_claim(struct vmw_device *v)
{
	/* Collect first, every time, rather than only when nothing is free.
	 *
	 * A buffer the device REFUSED is repaired and named here, and nowhere
	 * else that a thread can print from: the fence interrupt collects
	 * only what completed cleanly.  Reaping just before a claim fails
	 * would have left an error unreported until fifteen more buffers had
	 * gone by -- long enough for the log to name it beside completely
	 * unrelated work, which is the difference between a report that
	 * points at the guilty batch and one that misleads.  The cost is
	 * sixteen loads of a status word per submission, against two exits
	 * from the virtual machine. */
	uint64_t t0 = 0;

	cb_reap(v, 1);
	for (;;) {
		/* Slot 0 is reserved: see cb_slot_take(). */
		for (int i = 1; i < v->cb_nslots; i++) {
			struct vmw_cb_slot *s = &v->cb_slot[i];
			int st = CB_FREE;

			if (s->state != CB_FREE)
				continue;
			if (__atomic_compare_exchange_n(&s->state, &st,
							CB_FILLING, 0,
							__ATOMIC_ACQUIRE,
							__ATOMIC_RELAXED)) {
				return s;
			}
		}
		if (!t0) {
			t0 = timer_rdtsc();
		}
		cb_reap(v, 1);
		defer_drain(v);
		if (!can_sleep() ||
		    timer_rdtsc() - t0 < spin_ticks(VMW_SPIN_SLOT_US))
			__asm__ volatile("pause");
		else
			cb_slot_sleep(v);
	}
}

/* Take one specific slot, waiting for it.
 *
 * Slot 0 is kept for restarting a stopped context, and nothing else may
 * claim it.  The restart happens from inside the reaper, which is itself
 * reached from a submitter that could not find a free slot -- so a restart
 * that went looking for a slot of its own could re-enter the reaper looking
 * for one, which is a loop with the device's own recovery inside it.  A
 * slot that is never handed out settles it. */
static struct vmw_cb_slot *cb_slot_take(struct vmw_device *v, int index)
{
	struct vmw_cb_slot *s = &v->cb_slot[index];

	for (;;) {
		int st = CB_FREE;

		if (__atomic_compare_exchange_n(&s->state, &st, CB_FILLING, 0,
						__ATOMIC_ACQUIRE,
						__ATOMIC_RELAXED))
			return s;
		if (sched_current() && irqs_enabled())
			sched_yield_in_kernel();
		else
			__asm__ volatile("pause");
	}
}

/* A rejected buffer, while the slot still holds it.  Two distinct
 * rejections, two recoveries:
 *
 * A COMMAND error is not confined to the command that caused it: the device
 * HALTS the context it was submitted to.  From then on every buffer sent to
 * that context is left at SVGA_CB_STATUS_NONE.  Restarting the context is
 * what the guest is expected to do.  The commands are NOT resubmitted: the
 * device said they are wrong, and again would be again.
 *
 * A HEADER error names nothing inside the buffer -- it is how the device
 * answers a submission it will not look at, and the way to earn one with a
 * header that was fine a moment ago is for the device to have been reset
 * underneath the channel (a legacy mode set or a DPMS off does that).
 * Nothing is wrong with the commands, so the recovery is to start the
 * context again and resubmit the same payload -- once. */
/* One thread at a time repairs the channel.
 *
 * Repair preempts the context, rewrites the buffer that failed and hands
 * back every buffer the device returned unexecuted -- in the order they
 * were first given.  None of that survives two threads doing it at once,
 * and the claim is held across waits for the device, so it is a yielding
 * flag and not a spinlock (see cb_channel_acquire for what a spinlock held
 * across a device wait cost). */
static void cb_recover_lock(struct vmw_device *v)
{
	while (__atomic_test_and_set(&v->cb_recover_busy, __ATOMIC_ACQUIRE)) {
		if (sched_current() && irqs_enabled())
			sched_yield_in_kernel();
		else
			__asm__ volatile("pause");
	}
}

static void cb_recover_unlock(struct vmw_device *v)
{
	__atomic_clear(&v->cb_recover_busy, __ATOMIC_RELEASE);
}

/* Name the command the device would not take, and dump the batch around it.
 *
 * The named command is very often NOT the sick one: the device validates
 * bindings lazily, so a bad id planted by any earlier state command in the
 * batch only detonates at the DRAW.  So the whole batch goes out -- ids,
 * sizes and the leading body words hold every (post-relocation) object id
 * the draw depends on. */
static void cb_report_command_error(struct vmw_device *v, struct vmw_cb_slot *s)
{
	uint32_t off = s->hdr->errorOffset;

	(void)v;
	if (off + sizeof(SVGA3dCmdHeader) > s->bytes) {
		kprintf("[drm] vmwgfx: command error at offset %u\n", off);
		return;
	}
	const SVGA3dCmdHeader *bad = (const SVGA3dCmdHeader *)(s->buf + off);

	/* Rate-limited by COMMAND, not by a running count.
	 *
	 * A flat cap is the wrong shape here: one command the client repeats
	 * every frame spends the whole budget in the first second, and every
	 * DIFFERENT command the device refuses after that -- the ones nobody
	 * has seen yet -- is silent.  That is a log that hides exactly what it
	 * was added to show.  One line per distinct command id says everything
	 * once and repeats nothing. */
	static uint32_t seen[32];
	static int nseen;

	for (int i = 0; i < nseen; i++)
		if (seen[i] == bad->id)
			return;
	if (nseen < (int)(sizeof(seen) / sizeof(seen[0])))
		seen[nseen++] = bad->id;

	kprintf("[drm] vmwgfx: command error at offset %u: command %u, %u bytes\n",
		off, bad->id, bad->size);
	static int dumped;
	if (dumped >= 2)
		return;
	dumped++;
	uint32_t o = 0;
	while (o + sizeof(SVGA3dCmdHeader) <= s->bytes) {
		const SVGA3dCmdHeader *ch = (const SVGA3dCmdHeader *)(s->buf + o);
		const uint32_t *w = (const uint32_t *)(ch + 1);
		uint32_t room = (s->bytes - o - (uint32_t)sizeof(*ch)) / 4;
		uint32_t n = ch->size / 4;
		char t[6][12];

		/* A length that runs past the buffer is exactly what a
		 * malformed batch has; read only what is really there. */
		if (n > room)
			n = room;
		for (uint32_t k = 0; k < 6; k++) {
			if (k < n)
				ksnprintf(t[k], sizeof(t[k]), "%x", w[k]);
			else {
				t[k][0] = '.';
				t[k][1] = 0;
			}
		}
		kprintf("[drm]  %c off %u: cmd %u len %u | %s %s %s %s %s %s\n",
			o == off ? '>' : ' ', o, ch->id, ch->size, t[0], t[1],
			t[2], t[3], t[4], t[5]);
		if (ch->size % 4 || ch->size > s->bytes - o - sizeof(*ch))
			break;
		o += sizeof(*ch) + ch->size;
	}
}

/* Hand the device back every buffer it returned unexecuted, oldest first.
 *
 * `first' is the repaired buffer whose command caused the failure: it was
 * submitted before any of the ones the preempt handed back, so it goes
 * first.  Order is not a nicety -- these buffers define objects the ones
 * behind them use. */
static void cb_resubmit_preempted(struct vmw_device *v, uint32_t ctx,
				  struct vmw_cb_slot *first)
{
	struct vmw_cb_slot *q[VMW_CB_SLOTS];
	int n = 0;

	for (int i = 0; i < v->cb_nslots && n < VMW_CB_SLOTS; i++) {
		struct vmw_cb_slot *s = &v->cb_slot[i];
		int st = s->state;

		if (s == first || (st != CB_ASYNC && st != CB_SYNC))
			continue;
		if (s->ctx != ctx)
			continue;
		/* Only what the device gave back.  A buffer still at
		 * SVGA_CB_STATUS_NONE after the preempt is one the device
		 * has NOT let go of, and submitting it again would run it
		 * twice. */
		if (s->hdr->status != SVGA_CB_STATUS_PREEMPTED)
			continue;
		q[n++] = s;
	}
	/* Oldest first: an insertion sort over at most sixteen. */
	for (int i = 1; i < n; i++) {
		struct vmw_cb_slot *k = q[i];
		int j = i - 1;

		while (j >= 0 && q[j]->seq > k->seq) {
			q[j + 1] = q[j];
			j--;
		}
		q[j + 1] = k;
	}
	if (first)
		cb_slot_ring(v, first);
	for (int i = 0; i < n; i++) {
		q[i]->retried = 0;
		cb_slot_ring(v, q[i]);
	}
}

/* A buffer the device did not complete, while the slot still holds it.
 *
 * Returns 1 when the slot is in flight again -- the caller must neither
 * free it nor act on the status it read -- and 0 when it is finished for
 * good.  `s->err_rc' carries the device's verdict to a synchronous
 * submitter whose commands were the ones rejected.
 *
 * Three distinct answers, three recoveries:
 *
 * A COMMAND error is not confined to the command that caused it: the device
 * STOPS the context it was submitted to.  Every buffer already queued
 * behind it stays in the device's queue, untouched, and simply restarting
 * the context does not bring them back -- their status never leaves
 * SVGA_CB_STATUS_NONE and nothing here can tell them from work in
 * progress.  So the recovery is the reference driver's: PREEMPT the
 * context, which returns every buffer that had not begun; skip the one
 * command the device would not take; restart the context; and hand the
 * repaired buffer and everything that came back over again, in submission
 * order.
 *
 * Dropping them instead is what turned a single rejected command into a
 * storm.  Everything queued behind it went with it -- the surface defines,
 * the MOB destroys, and the SVGA_CMD_FENCE that ends every batch -- so the
 * ids those commands would have created were rejected in turn, the ids
 * they would have released were still taken when the allocator handed them
 * out again, and every fence waiter sat out its full timeout.  One command
 * the device would not take cost the whole 3D path.
 *
 * A HEADER error names nothing inside the buffer -- it is how the device
 * answers a submission it will not look at, and the way to earn one with a
 * header that was fine a moment ago is for the device to have been reset
 * underneath the channel (a legacy mode set or a DPMS off does that).
 * Nothing is wrong with the commands, so the recovery is to start the
 * context again and resubmit the same payload -- once.
 *
 * A buffer the device has not touched for the whole timeout is the
 * watchdog's: preempt to get it back, restart, and give it one more go. */
static int cb_slot_handle_error(struct vmw_device *v, struct vmw_cb_slot *s,
				int timed_out)
{
	volatile SVGACBHeader *h = s->hdr;

	cb_recover_lock(v);
	uint32_t status = h->status;

	/* Another thread may have repaired the channel while this one waited
	 * for the claim -- and repairing it re-rings the buffers it took
	 * back, this one possibly among them.  Say "in flight again": the
	 * caller looks at the status once more instead of acting on the one
	 * it read before the claim. */
	if (status == SVGA_CB_STATUS_COMPLETED ||
	    (status == SVGA_CB_STATUS_NONE && !timed_out)) {
		cb_recover_unlock(v);
		return 1;
	}

	/* A device-context buffer (start/stop, preempt) has no context to
	 * restart and no command stream to skip into. */
	if (s->ctx == SVGA_CB_CONTEXT_DEVICE) {
		kprintf("[drm] vmwgfx: device command buffer refused (status %u)\n",
			status);
		s->err_rc = cb_status_rc(status);
		cb_recover_unlock(v);
		return 0;
	}

	if (status == SVGA_CB_STATUS_NONE) {
		/* The watchdog: nothing at all has finished for the timeout,
		 * so this is a wedged channel and not a busy one. */
		kprintf("[drm] vmwgfx: device made no progress for 2s (ctx %u, %u bytes); restarting\n",
			s->ctx, s->bytes);
		cb_preempt_context(v, s->ctx);
		cb_start_context(v, s->ctx);
		v->cb_last_progress_us = timer_get_precise_us();
		status = h->status;
		if (status == SVGA_CB_STATUS_COMPLETED) {
			cb_resubmit_preempted(v, s->ctx, NULL);
			cb_recover_unlock(v);
			return 1;
		}
		if (status == SVGA_CB_STATUS_PREEMPTED && !s->retried) {
			s->retried = 1;
			cb_resubmit_preempted(v, s->ctx, s);
			cb_recover_unlock(v);
			return 1;
		}
		cb_resubmit_preempted(v, s->ctx, NULL);
		s->err_rc = -ETIMEDOUT;
		cb_recover_unlock(v);
		return 0;
	}

	if (status != SVGA_CB_STATUS_COMMAND_ERROR) {
		/* QUEUE_FULL, PREEMPTED, CB_HEADER_ERROR, SUBMISSION_ERROR:
		 * nothing is wrong with the COMMANDS.  Restart the channel
		 * where the device stopped it and hand the same payload over
		 * again. */
		if (status != SVGA_CB_STATUS_QUEUE_FULL &&
		    status != SVGA_CB_STATUS_PREEMPTED) {
			static int reported;

			if (reported < 3) {
				reported++;
				kprintf("[drm] vmwgfx: channel error %u (len %u flags %u dx %u); restarting\n",
					status, s->bytes, s->flags, s->dx);
			}
			cb_start_context(v, s->ctx);
		}
		/* A preempt is this driver's own doing, never the buffer's
		 * fault, so it does not spend the one retry. */
		if (!s->retried || status == SVGA_CB_STATUS_PREEMPTED) {
			if (status != SVGA_CB_STATUS_PREEMPTED)
				s->retried = 1;
			cb_slot_ring(v, s); /* same payload */
			cb_recover_unlock(v);
			return 1; /* in flight again: do NOT free the slot */
		}
		kprintf("[drm] vmwgfx: command-buffer channel did not recover (status %u)\n",
			status);
		s->err_rc = cb_status_rc(status);
		cb_recover_unlock(v);
		return 0;
	}

	cb_report_command_error(v, s);
	/* The verdict belongs to whoever submitted this buffer and waits for
	 * an answer -- vmw_context_cotable_reserve() acts on it.  The
	 * remainder of the batch is still handed back below; the answer is
	 * about the command that was dropped. */
	s->err_rc = -EINVAL;

	/* Everything the device still holds on this context, back. */
	cb_preempt_context(v, s->ctx);

	/* What is left of this buffer past the command the device refused.
	 *
	 * How long that command is depends on which stream it belongs to,
	 * and the id says which: at or above SVGA_CMD_MAX it is an SVGA3D
	 * command, whose header carries its own length; below that it is a
	 * FIFO-format command whose length is implied by the id, and of
	 * those only SVGA_CMD_FENCE ever reaches these buffers from this
	 * driver.  Anything else cannot be measured, so nothing past it can
	 * be found and the rest of the buffer is given up.  (The reference
	 * driver's vmw_cmd_describe() draws the same line at the same
	 * place.) */
	uint32_t off = h->errorOffset;
	uint32_t new_start = s->bytes;

	if (off + sizeof(uint32_t) <= s->bytes) {
		uint32_t id = *(const uint32_t *)(s->buf + off);
		uint32_t avail = s->bytes - off;

		if (id >= SVGA_CMD_MAX) {
			if (avail >= sizeof(SVGA3dCmdHeader)) {
				const SVGA3dCmdHeader *bad =
					(const SVGA3dCmdHeader *)(s->buf + off);
				uint32_t body = avail - (uint32_t)sizeof(*bad);

				if (!(bad->size & 3) && bad->size <= body)
					new_start = off + (uint32_t)sizeof(*bad) +
						    bad->size;
			}
		} else if (id == SVGA_CMD_FENCE && avail >= 2 * sizeof(uint32_t)) {
			new_start = off + 2 * (uint32_t)sizeof(uint32_t);
		}
	}

	cb_start_context(v, s->ctx);
	v->cb_last_progress_us = timer_get_precise_us();

	if (new_start < s->bytes) {
		/* dst below src, and mm_memcpy copies forward.  The buffer
		 * strictly shrinks, so a batch where every command is
		 * refused ends rather than going round for ever. */
		mm_memcpy(s->buf, s->buf + new_start, s->bytes - new_start);
		s->bytes -= new_start;
		s->retried = 0;
	} else if (!s->retried) {
		/* Nothing of the buffer survives -- and the SVGA_CMD_FENCE
		 * that ended the batch went with it.  Put a fresh fence in
		 * its place: a number above every one handed out so far, so
		 * every waiter for a fence that can no longer arrive is
		 * released rather than sitting out its timeout.  (The
		 * reference driver sends a fence here for the same reason.)
		 *
		 * Once only.  A device that refuses even this has nothing
		 * left to be salvaged into, and installing another fence
		 * each time round is a loop with no end. */
		uint32_t seq = vmsvga2_fence_alloc();

		if (!seq) {
			cb_resubmit_preempted(v, s->ctx, NULL);
			cb_recover_unlock(v);
			return 0;
		}
		uint32_t f[2] = { SVGA_CMD_FENCE, seq };

		mm_memcpy(s->buf, f, sizeof(f));
		s->bytes = (uint32_t)sizeof(f);
		s->flags = SVGA_CB_FLAG_NONE;
		s->dx = 0;
		s->fence_seq = seq; /* what this buffer now stands for */
		s->retried = 1;
	} else {
		cb_resubmit_preempted(v, s->ctx, NULL);
		cb_recover_unlock(v);
		return 0;
	}
	cb_resubmit_preempted(v, s->ctx, s);
	cb_recover_unlock(v);
	return 1;
}

/* Submit one buffer and wait for the device to finish it. */
static int cb_submit_slot(struct vmw_device *v, struct vmw_cb_slot *s,
			  const void *payload, uint32_t bytes, uint32_t flags,
			  uint32_t dx_cid, uint32_t ctx)
{
	mm_memcpy(s->buf, payload, bytes);
	s->bytes = bytes;
	s->flags = flags;
	s->dx = dx_cid;
	s->ctx = ctx;
	s->retried = 0;
	s->err_rc = 0;
	s->fence_seq = 0;
	cb_slot_ring(v, s);
	/* This submitter owns the slot until it is done: reapers skip it. */
	__atomic_store_n(&s->state, CB_SYNC, __ATOMIC_RELEASE);
	volatile SVGACBHeader *h = s->hdr;
	int rc;
	for (;;) {
		uint32_t status = h->status;

		if (status == SVGA_CB_STATUS_COMPLETED) {
			v->cb_last_progress_us = timer_get_precise_us();
			cb_slot_completed(v, s);
			/* Completed -- but possibly a REPAIRED buffer: the
			 * device refused one of these commands and the rest
			 * were handed back without it.  The caller asked
			 * because it acts on the answer, so it gets the
			 * device's verdict and not the repair's. */
			rc = s->err_rc;
			break;
		}
		if (status == SVGA_CB_STATUS_NONE) {
			if (!cb_deadline_passed(v, s->submitted_us)) {
				/* Yield only where yielding is allowed;
				 * before there is a scheduler, spin -- the
				 * device answers in microseconds. */
				if (sched_current() && irqs_enabled())
					sched_yield_in_kernel();
				else
					__asm__ volatile("pause");
				continue;
			}
			if (cb_slot_handle_error(v, s, 1))
				continue; /* back with the device; wait again */
			rc = s->err_rc ? s->err_rc : -ETIMEDOUT;
			break;
		}
		if (cb_slot_handle_error(v, s, 0))
			continue; /* resubmitted; wait again */
		rc = s->err_rc ? s->err_rc : cb_status_rc(h->status);
		break;
	}
	cb_slot_completed(v, s); /* nothing left to wait for, either way */
	__atomic_store_n(&s->state, CB_FREE, __ATOMIC_RELEASE);
	return rc;
}

static int cb_submit_sync(struct vmw_device *v, const void *payload,
			  uint32_t bytes, uint32_t flags, uint32_t dx_cid,
			  uint32_t ctx)
{
	return cb_submit_slot(v, cb_slot_claim(v), payload, bytes, flags,
			      dx_cid, ctx);
}

/* Submit one buffer and return; completion is collected by cb_reap().
 * `fence_seq' is the SVGA_CMD_FENCE sequence the payload ends with, or 0:
 * it is what cb_slot_completed() signals when the device is done. */
static int cb_submit_async_seq(struct vmw_device *v, const void *payload,
			       uint32_t bytes, uint32_t flags, uint32_t dx_cid,
			       uint32_t ctx, uint32_t fence_seq)
{
	struct vmw_cb_slot *s = cb_slot_claim(v);

	mm_memcpy(s->buf, payload, bytes);
	s->bytes = bytes;
	s->flags = flags;
	s->dx = dx_cid;
	s->ctx = ctx;
	s->retried = 0;
	s->err_rc = 0;
	s->fence_seq = fence_seq;
	cb_slot_ring(v, s);
	/* Only now may anyone else look at it: the header describes THIS
	 * submission and the device has been told about it. */
	__atomic_store_n(&s->state, CB_ASYNC, __ATOMIC_RELEASE);
	return 0;
}

static int cb_submit_async(struct vmw_device *v, const void *payload,
			   uint32_t bytes, uint32_t flags, uint32_t dx_cid,
			   uint32_t ctx)
{
	return cb_submit_async_seq(v, payload, bytes, flags, dx_cid, ctx, 0);
}

/* Page tables the device has been told to stop using.
 *
 * The command saying so is queued like everything else, so the pages cannot
 * be handed back at once -- the device may not have read it yet.  They are
 * held until every buffer given to the device has been collected, which
 * means it has worked through the command as well.  Waiting for that on the
 * spot is what this replaces: it happened per buffer destroyed, and a
 * display server destroys one per pixmap. */
/* Enough entries to hold a quarter of a gigabyte of pages waiting on the
 * device.  Past that something is wrong with the device rather than with the
 * client, and making it idle is the right answer. */
#define VMW_DEFER_CAP_MAX (1u << 16)
#define VMW_DEFER_CAP_MIN 64u

/* Make room for `need' more pages, growing the holding area if necessary.
 *
 * The allocation happens with the lock DROPPED -- it may sleep -- so the
 * capacity is re-tested after retaking it and a loser frees its spare copy.
 * Returns false only when the area cannot grow, and the caller then has to
 * make the device idle instead. */
static bool defer_reserve(struct vmw_device *v, uint32_t need)
{
	uint64_t fl;

	for (;;) {
		uint32_t want, have_n;
		uint64_t *nw, *old;

		spin_lock_irqsave(&v->defer_lock, &fl);
		if (v->defer_n + need <= v->defer_cap) {
			spin_unlock_irqrestore(&v->defer_lock, fl);
			return true;
		}
		have_n = v->defer_n;
		spin_unlock_irqrestore(&v->defer_lock, fl);

		want = v->defer_cap ? v->defer_cap : VMW_DEFER_CAP_MIN;
		while (want < have_n + need) {
			if (want > VMW_DEFER_CAP_MAX / 2)
				return false;
			want *= 2;
		}
		nw = kalloc((size_t)want * sizeof(*nw));
		if (!nw)
			return false;

		spin_lock_irqsave(&v->defer_lock, &fl);
		if (v->defer_n + need <= v->defer_cap) {
			/* someone else grew it while this allocated */
			spin_unlock_irqrestore(&v->defer_lock, fl);
			kfree(nw);
			return true;
		}
		if (want < v->defer_n + need) {
			/* it filled further meanwhile: size it again */
			spin_unlock_irqrestore(&v->defer_lock, fl);
			kfree(nw);
			continue;
		}
		for (uint32_t i = 0; i < v->defer_n; i++)
			nw[i] = v->defer_free[i];
		old = v->defer_free;
		v->defer_free = nw;
		v->defer_cap = want;
		spin_unlock_irqrestore(&v->defer_lock, fl);
		kfree(old);
		return true;
	}
}

/* Put one page in the holding area, or free it the slow way if it is full.
 *
 * The append is under `defer_lock' because every processor destroying a
 * buffer object arrives here at once: unlocked, two of them read the same
 * `defer_n', wrote the same slot and advanced the count by one between them,
 * so one page was overwritten and never freed -- a leak proportional to how
 * much the client draws, which is what filling memory while browsing looked
 * like from the outside.
 *
 * vmw_cmd_flush()/cb_reap() are called with the lock NOT held: they talk to
 * the device and can spin for milliseconds. */
static void defer_free_page(struct vmw_device *v, uint64_t phys)
{
	uint64_t fl;
	int queued = 0;

	/* Try to hand back what is already held first: this runs on a thread
	 * tearing a buffer down, which is exactly when the device is likely
	 * to have caught up.  Without it the list only ever emptied when it
	 * overflowed, since the other place that drains it runs when no slot
	 * is free -- precisely when the device has NOT caught up. */
	defer_drain(v);

	if (defer_reserve(v, 1)) {
		spin_lock_irqsave(&v->defer_lock, &fl);
		if (v->defer_n < v->defer_cap) {
			v->defer_free[v->defer_n++] = phys;
			queued = 1;
		}
		spin_unlock_irqrestore(&v->defer_lock, fl);
	}
	if (queued)
		return;

	/* Nowhere to hold it, so the page can only be released by making sure
	 * the device is finished with it -- and that means WAITING.
	 *
	 * vmw_cmd_flush() only hands the gathered commands over and cb_reap()
	 * only collects buffers that have already finished; neither of them
	 * waits for anything, so between them they were no guarantee at all.
	 * This path is reached exactly when the holding area is full, which is
	 * when the client is churning buffers hardest and the device is
	 * furthest behind -- the worst moment to hand a page it is still
	 * reading back to the allocator, where the next allocation writes over
	 * what the device is about to display.
	 *
	 * vmw_cmd_drain() flushes, waits for every slot to fall idle, and
	 * drains the holding area on its way out -- so afterwards nothing is
	 * in flight and this page, and everything that was held, are all
	 * genuinely free. */
	vmw_cmd_drain(v);
	mm_free_physical_page(phys);
}

/* Hand back everything the device has finished with.
 *
 * The pages are taken OUT of the array under the lock and freed after it is
 * dropped, which is what makes concurrent drains safe: whoever empties the
 * array owns exactly what it took, and a second drain arriving behind it
 * finds the count at zero and frees nothing.  Reading the count under no lock
 * and freeing in place -- what this did before -- let two processors free the
 * same pages, and a page freed twice is handed out to two owners at once.
 * That is where the `double-free of page' warnings and the poisoned pointers
 * in unrelated kernel structures came from. */
static void defer_drain(struct vmw_device *v)
{
	uint64_t *pages;
	uint32_t n;
	uint64_t fl;

	if (!v->defer_n || v->pend_len)
		return;
	for (int i = 0; i < v->cb_nslots; i++)
		if (v->cb_slot[i].state != CB_FREE)
			return; /* something is still in flight */

	/* The whole array is taken, not copied into one on the stack: it is
	 * grown to whatever a client's buffers needed and no longer has a
	 * bound small enough to stand there. */
	spin_lock_irqsave(&v->defer_lock, &fl);
	pages = v->defer_free;
	n = v->defer_n;
	v->defer_free = NULL;
	v->defer_n = 0;
	v->defer_cap = 0;
	spin_unlock_irqrestore(&v->defer_lock, fl);

	for (uint32_t i = 0; i < n; i++)
		mm_free_physical_page(pages[i]);
	kfree(pages);
}

/* Release the pages that backed a buffer object.
 *
 * These are the pages whose frame numbers were written into a MOB page table
 * and handed to the HOST -- see vmw_mob_bind().  The device reaches them by
 * walking that table, so they may not go back to the allocator while it is
 * still doing so, and destroying the MOB does not by itself stop it: the
 * DESTROY_GB_MOB command is queued like any other and the device gets to it
 * when it gets to it.
 *
 * A page released early does not come back as a fault or an error.  It comes
 * back as a page of someone else's memory that the host is still writing
 * into: a library's text overwritten with object-table entries, or a heap
 * whose chunk headers stop making sense.  Nothing on this side can catch it,
 * because no processor performs the write.
 *
 * The rule the reference keeps is that a buffer's pages are freed only once
 * the device is idle with respect to that buffer, and that a buffer which is
 * not idle has its release deferred rather than forced.  Both halves are
 * here: what fits in the holding area waits there for the device to drain on
 * its own, and only when an object is too big for that is the device made
 * idle -- once for the whole object, never once per page. */
void vmw_defer_free_pages(struct vmw_device *v, const uint64_t *pages,
			  uint32_t n)
{
	uint32_t i = 0;

	if (!v || !pages)
		return;

	defer_drain(v);

	/* One critical section for the whole buffer: the count must not move
	 * between the test and the store, or two callers claim one slot. */
	if (defer_reserve(v, n)) {
		uint64_t fl;

		spin_lock_irqsave(&v->defer_lock, &fl);
		for (; i < n && v->defer_n < v->defer_cap; i++)
			if (pages[i])
				v->defer_free[v->defer_n++] = pages[i];
		spin_unlock_irqrestore(&v->defer_lock, fl);
	}
	if (i == n)
		return;

	/* Nowhere to hold the rest, so the only way they can go back is to
	 * make the device finish with them first -- once, here, for whatever
	 * is left of this buffer.
	 *
	 * This used to flush and reap and then free them regardless.  Neither
	 * of those waits for anything: vmw_cmd_flush() hands the gathered
	 * commands over and cb_reap() collects what has already finished, so
	 * the pages went back to the allocator while the host could still be
	 * reading them.  A full-screen image put 2186 of its 2250 pages
	 * through that path every time one was torn down. */
	vmw_cmd_drain(v);
	for (; i < n; i++)
		if (pages[i])
			mm_free_physical_page(pages[i]);
}

/* Reap entry point for the fence tick: collect finished buffers so errors
 * surface promptly even when no submitter comes by. */
void vmw_cmdbuf_poll(struct vmw_device *v)
{
	/* Reached from the fence interrupt: collect only.  Recovery and the
	 * deferred frees belong to a thread. */
	if (v->cb_ready)
		cb_reap(v, 0);
}

/* A device-context command -- start or stop a context, preempt one -- on the
 * reserved slot, waited for.
 *
 * Deliberately NOT routed through cb_submit_slot(): the repair below is
 * built out of these, and a repair that repairs by repairing has nowhere to
 * stop.  A device-context buffer the device refuses is reported and given
 * up on; there is no context behind it to restart. */
static int cb_dc_command(struct vmw_device *v, const void *cmd, uint32_t bytes)
{
	struct vmw_cb_slot *s = cb_slot_take(v, 0);

	mm_memcpy(s->buf, cmd, bytes);
	s->bytes = bytes;
	s->flags = SVGA_CB_FLAG_NONE;
	s->dx = 0;
	s->ctx = SVGA_CB_CONTEXT_DEVICE;
	s->retried = 0;
	s->err_rc = 0;
	s->fence_seq = 0;
	cb_slot_ring(v, s);
	__atomic_store_n(&s->state, CB_SYNC, __ATOMIC_RELEASE);
	volatile SVGACBHeader *h = s->hdr;
	int rc;
	for (;;) {
		uint32_t status = h->status;

		if (status != SVGA_CB_STATUS_NONE) {
			rc = cb_status_rc(status);
			if (rc)
				kprintf("[drm] vmwgfx: device command %u refused (status %u)\n",
					*(const uint32_t *)cmd, status);
			else
				v->cb_last_progress_us = timer_get_precise_us();
			break;
		}
		if (cb_deadline_passed(v, s->submitted_us)) {
			rc = -ETIMEDOUT;
			break;
		}
		if (sched_current() && irqs_enabled())
			sched_yield_in_kernel();
		else
			__asm__ volatile("pause");
	}
	__atomic_store_n(&s->state, CB_FREE, __ATOMIC_RELEASE);
	return rc;
}

static int cb_start_context(struct vmw_device *v, uint32_t ctx)
{
	struct {
		uint32_t id;
		SVGADCCmdStartStop body;
	} cmd;

	cmd.id = SVGA_DC_CMD_START_STOP_CONTEXT;
	cmd.body.enable = 1;
	cmd.body.context = (SVGACBContext)ctx;
	return cb_dc_command(v, &cmd, (uint32_t)sizeof(cmd));
}

/* Take back every buffer the device still has queued on `ctx'.
 *
 * After a command error the context is STOPPED and the buffers behind the
 * failure sit in the device's queue untouched: their status never leaves
 * SVGA_CB_STATUS_NONE, so nothing here can tell them from a buffer the
 * device is still working through, and restarting the context does not
 * bring them back.  Preempting does -- every buffer that had not begun
 * comes back marked PREEMPTED, and the guest submits it again.  This is
 * what the reference driver does at exactly this point. */
static int cb_preempt_context(struct vmw_device *v, uint32_t ctx)
{
	struct {
		uint32_t id;
		SVGADCCmdPreempt body;
	} cmd;

	cmd.id = SVGA_DC_CMD_PREEMPT;
	cmd.body.context = (SVGACBContext)ctx;
	cmd.body.ignoreIDZero = 0;
	return cb_dc_command(v, &cmd, (uint32_t)sizeof(cmd));
}

/* Emit a fence for everything queued so far, and return its sequence.
 *
 * The number is allocated and the buffer carrying it handed over under ONE
 * hold of the gather claim, so the order the numbers are handed out is the
 * order the device is told about them.  Allocating first and submitting
 * afterwards is not the same thing: two threads then swap, the higher
 * number completes first, and completion of the higher signals the lower --
 * whose work has not run.
 *
 * The fence travels down THIS channel, gathered with everything before it
 * and then handed over, because a fence means nothing until the commands it
 * stands for have gone.  What tells the driver it has passed is the
 * COMPLETION of the buffer it is in; see cb_slot_completed(). */
uint32_t vmw_cmd_fence_emit(struct vmw_device *v)
{
	uint32_t cmd[2];
	uint32_t seq;

	if (!v->cb_ready)
		return 0;
	if (!v->pend) {
		seq = vmsvga2_fence_alloc();
		if (!seq)
			return 0;
		cmd[0] = SVGA_CMD_FENCE;
		cmd[1] = seq;
		if (cb_submit_async_seq(v, cmd, (uint32_t)sizeof(cmd),
					SVGA_CB_FLAG_NONE, 0, SVGA_CB_CONTEXT_0,
					seq) != 0)
			return 0;
		return seq;
	}

	pend_lock(v);
	seq = vmsvga2_fence_alloc();
	if (seq) {
		if (v->pend_len) {
			/* TAG the buffer the work is already in, rather than
			 * handing that buffer over and then a second one
			 * carrying eight bytes of SVGA_CMD_FENCE.
			 *
			 * Nothing here ever reads the device's fence
			 * register -- vmw_fence_check() says why, at length --
			 * so the FENCE COMMAND was never what told this driver
			 * the fence had passed.  The COMPLETION of the buffer
			 * is (cb_slot_completed), and the slot carries the
			 * sequence out of band.  So the command was pure cost:
			 * a whole second command buffer, a second pair of
			 * doorbell register writes, and a second exit to the
			 * host, for every submission -- and a submission is
			 * every execbuf, eight to twelve a frame.
			 *
			 * It is also more accurate this way.  The fence now
			 * passes when the batch it stands for completes,
			 * instead of when a buffer queued behind that batch
			 * does. */
			v->pend_fence_seq = seq;
			if (pend_flush_locked(v) != 0)
				seq = 0;
		} else {
			/* Nothing gathered: everything this fence stands for
			 * has already gone, so it needs a buffer of its own to
			 * complete behind them.  One context executes its
			 * buffers in order, so completion of this one means
			 * completion of all of them. */
			cmd[0] = SVGA_CMD_FENCE;
			cmd[1] = seq;
			if (cb_submit_async_seq(v, cmd, (uint32_t)sizeof(cmd),
						SVGA_CB_FLAG_NONE, 0,
						SVGA_CB_CONTEXT_0, seq) != 0)
				seq = 0;
		}
	}
	pend_unlock(v);
	return seq;
}

/* Device-format bytes -- SVGA_CMD_*, not SVGA3D commands -- down whichever
 * channel this driver owns.
 *
 * This is the reference driver's VMW_CMD_RESERVE: with a command-buffer
 * manager, the FIFO is not used for commands AT ALL.  Everything goes down
 * the one channel -- FIFO-format commands, SVGA3D commands and the fence
 * alike -- and that is not tidiness, it is the two things a display needs:
 *
 * - ORDER.  The screen blits below name a guest memory region the
 *   command-buffer channel has been filling.  Two streams have no order
 *   between them, so the blit can reach the host before the drawing it is
 *   supposed to show, and the screen shows the frame before.
 * - ONE FENCE WRITER.  The device has a single fence register.  A fence in
 *   the FIFO and a fence in a command buffer both advance it, so a value on
 *   it means "somebody got this far" rather than "this stream did", and
 *   every wait returns for the wrong reason.
 *
 * `ring' is the FIFO's batching hint: 0 means a run of these is coming and
 * only the last one need be announced.  Gathering does the same thing for a
 * command buffer, so it maps straight across.
 */
int vmw_cmd_raw(struct vmw_device *v, const void *cmds, uint32_t bytes, int ring)
{
	if (bytes == 0)
		return 0;
	if (v->cb_ready) {
		int rc;

		if (v->pend && bytes <= VMW_PEND_BYTES) {
			rc = pend_add(v, (const uint8_t *)cmds, bytes,
				      SVGA3D_INVALID_ID);
			if (rc == 0 && ring)
				vmw_cmd_flush(v);
			return rc;
		}
		/* Too big to gather: hand over what is gathered first, so
		 * this still follows what came before it. */
		vmw_cmd_flush(v);
		if (bytes <= v->cb_size)
			return cb_submit_async(v, cmds, bytes,
					       SVGA_CB_FLAG_NONE, 0,
					       SVGA_CB_CONTEXT_0);
	}
	if (!ring)
		return vmsvga2_hw_fifo_submit_batch(cmds, bytes) == 0 ? 0 : -EIO;
	return vmsvga2_hw_fifo_submit(cmds, bytes) == 0 ? 0 : -EIO;
}

/* The gather buffer.
 *
 * A command handed to the device costs a buffer and two register writes
 * that leave the virtual machine, and only so many buffers can be in flight
 * at once.  Sending each command on its own therefore ran out of buffers
 * after a handful of them -- and a display server issues several per pixmap
 * -- so every submitter then waited for the device to drain.  That is the
 * stutter: fast while buffers last, stalled while they are collected, fast
 * again.
 *
 * So commands are gathered and handed over together, which is what the
 * reference implementation's command-buffer manager does.  What forces a
 * hand-over: no more room, a change of DX context (one buffer carries one),
 * a caller that needs the answer, a fence (which must follow what came
 * before it), and a finished screen update (which has to reach the
 * display). */
static void pend_lock(struct vmw_device *v)
{
	uint64_t t0 = 0;

	while (__atomic_test_and_set(&v->pend_busy, __ATOMIC_ACQUIRE)) {
		if (!t0)
			t0 = timer_rdtsc();
		/* Spin for the ordinary hold; yield only for one that has
		 * outlived it -- the holder asleep for a slot, typically.
		 * See the note above cb_slot_claim(). */
		if (!can_sleep() ||
		    timer_rdtsc() - t0 < spin_ticks(VMW_SPIN_LOCK_US))
			__asm__ volatile("pause");
		else
			sched_yield_in_kernel();
	}
}

static void pend_unlock(struct vmw_device *v)
{
	__atomic_clear(&v->pend_busy, __ATOMIC_RELEASE);
}

/* Hand over whatever has been gathered.  Called with the gather lock held. */
static int pend_flush_locked(struct vmw_device *v)
{
	if (!v->pend_len)
		return 0;
	uint32_t bytes = v->pend_len;
	uint32_t dx = v->pend_dx;
	uint32_t seq = v->pend_fence_seq;
	v->pend_len = 0;
	v->pend_dx = SVGA3D_INVALID_ID;
	v->pend_fence_seq = 0;
	uint32_t flags = dx != SVGA3D_INVALID_ID ? SVGA_CB_FLAG_DX_CONTEXT :
						   SVGA_CB_FLAG_NONE;
	return cb_submit_async_seq(v, v->pend, bytes, flags,
				   dx != SVGA3D_INVALID_ID ? dx : 0,
				   SVGA_CB_CONTEXT_0, seq);
}

/* Hand over everything gathered and wait until the device has worked
 * through all of it.
 *
 * For the rare moment when memory the device is reading is about to be
 * replaced or read by the processor.  Queued submission means earlier
 * batches may still be executing, and they were built against the object
 * that is about to change -- the reference implementation waits for the
 * buffer to go idle at exactly these points, and this is that wait. */
void vmw_cmd_drain(struct vmw_device *v)
{
	if (!v->cb_ready)
		return;
	vmw_cmd_flush(v);
	/* From here, not from whenever the device last finished something:
	 * see cb_deadline_passed().  A drain that begins after an idle spell
	 * used to give up before it had waited at all. */
	uint64_t started_us = timer_get_precise_us();
	for (;;) {
		int busy = 0;

		cb_reap(v, 1);
		for (int i = 0; i < v->cb_nslots; i++)
			if (v->cb_slot[i].state != CB_FREE) {
				busy = 1;
				break;
			}
		if (!busy)
			break;
		if (cb_deadline_passed(v, started_us))
			break; /* the device is not moving; do not wait for ever */
		if (sched_current() && irqs_enabled())
			sched_yield_in_kernel();
		else
			__asm__ volatile("pause");
	}
	defer_drain(v);
}

void vmw_cmd_flush(struct vmw_device *v)
{
	if (!v->cb_ready || !v->pend)
		return;
	pend_lock(v);
	pend_flush_locked(v);
	pend_unlock(v);
}

/* Add to the gather buffer, handing over first if this cannot join what is
 * already there. */
static int pend_add(struct vmw_device *v, const uint8_t *p, uint32_t bytes,
		    uint32_t dx_cid)
{
	int rc = 0;

	pend_lock(v);
	if (v->pend_len && v->pend_dx != dx_cid)
		rc = pend_flush_locked(v);
	if (rc == 0 && v->pend_len + bytes > VMW_PEND_BYTES)
		rc = pend_flush_locked(v);
	if (rc == 0) {
		mm_memcpy(v->pend + v->pend_len, p, bytes);
		v->pend_len += bytes;
		v->pend_dx = dx_cid;
	}
	pend_unlock(v);
	return rc;
}

static int vmw_cmd_submit_flags(struct vmw_device *v, const void *cmds,
				uint32_t bytes, uint32_t dx_cid, int sync)
{
	if (bytes == 0)
		return 0;
	if (!v->has_cmdbuf || !v->cb_ready) {
		if (dx_cid != SVGA3D_INVALID_ID)
			return -ENODEV; /* DX needs command buffers */
		/* Chunk through the FIFO. */
		const uint8_t *p = cmds;
		while (bytes) {
			/* Whole commands only: walk headers. */
			uint32_t chunk = 0;
			while (chunk < bytes) {
				const SVGA3dCmdHeader *h = (const SVGA3dCmdHeader *)(p + chunk);
				uint32_t n = sizeof(*h) + h->size;
				if (chunk + n > bytes)
					return -EINVAL;
				if (chunk && chunk + n > 64 * 1024)
					break;
				chunk += n;
			}
			if (vmsvga2_hw_fifo_submit(p, chunk) != 0)
				return -EIO;
			p += chunk;
			bytes -= chunk;
		}
		return 0;
	}

	/* Gathered, unless the caller waits for it: then everything already
	 * gathered goes first, so the device sees it all in order, and this
	 * one is handed over on its own and waited for. */
	if (!sync && v->pend && bytes <= VMW_PEND_BYTES)
		return pend_add(v, cmds, bytes, dx_cid);
	if (v->pend) {
		pend_lock(v);
		int frc = pend_flush_locked(v);
		pend_unlock(v);
		if (frc)
			return frc;
	}

	const uint8_t *p = cmds;
	int rc = 0;
	while (bytes && rc == 0) {
		uint32_t chunk = bytes > v->cb_size ? v->cb_size : bytes;
		/* Never split a command. */
		if (chunk < bytes) {
			uint32_t at = 0;
			while (at < chunk) {
				const SVGA3dCmdHeader *h = (const SVGA3dCmdHeader *)(p + at);
				uint32_t n = sizeof(*h) + h->size;
				if (at + n > chunk)
					break;
				at += n;
			}
			chunk = at;
			if (!chunk) {
				rc = -EINVAL;
				break;
			}
		}
		uint32_t dx = dx_cid != SVGA3D_INVALID_ID ? dx_cid : 0;
		uint32_t flags = dx_cid != SVGA3D_INVALID_ID ?
					 SVGA_CB_FLAG_DX_CONTEXT :
					 SVGA_CB_FLAG_NONE;
		/* A multi-chunk batch stays ordered either way: one context,
		 * one queue.  Only the LAST chunk of a synchronous batch is
		 * waited for -- completion of the last implies the rest. */
		int last = chunk == bytes;
		if (sync && last)
			rc = cb_submit_sync(v, p, chunk, flags, dx,
					    SVGA_CB_CONTEXT_0);
		else
			rc = cb_submit_async(v, p, chunk, flags, dx,
					     SVGA_CB_CONTEXT_0);
		p += chunk;
		bytes -= chunk;
	}
	return rc;
}

int vmw_cmd_submit(struct vmw_device *v, const void *cmds, uint32_t bytes,
		   uint32_t dx_cid)
{
	return vmw_cmd_submit_flags(v, cmds, bytes, dx_cid, 1);
}

int vmw_cmd_submit_async(struct vmw_device *v, const void *cmds, uint32_t bytes,
			 uint32_t dx_cid)
{
	return vmw_cmd_submit_flags(v, cmds, bytes, dx_cid, 0);
}

static int vmw_cmd_one_flags(struct vmw_device *v, uint32_t id, const void *body,
			     uint32_t body_size, int sync)
{
	uint8_t buf[256];

	if (body_size + sizeof(SVGA3dCmdHeader) > sizeof(buf))
		return -EINVAL;
	SVGA3dCmdHeader *h = (SVGA3dCmdHeader *)buf;
	h->id = id;
	h->size = body_size;
	mm_memcpy(buf + sizeof(*h), body, body_size);
	return vmw_cmd_submit_flags(v, buf, (uint32_t)(sizeof(*h) + body_size),
				    SVGA3D_INVALID_ID, sync);
}

/* Queue one command and return.
 *
 * This is the default because it is what the object lifecycle needs: a
 * display server creates and destroys surfaces and their backing
 * continuously -- several commands per pixmap, dozens of pixmaps in the
 * time it takes to drag a window across the screen -- and each one waited
 * for the host to execute it before the next could be built.  All of that
 * happens on the server's own thread, and a thread parked in a driver
 * cannot read the mouse, so the pointer stopped whenever anything was
 * drawn.  One context executes in submission order, so queueing loses no
 * ordering; a command the device rejects is named in the log when its
 * buffer is collected.
 *
 * vmw_cmd_one_sync() is for the few callers that must know the answer, or
 * that are about to touch memory the device was told to read. */
int vmw_cmd_one(struct vmw_device *v, uint32_t id, const void *body,
		uint32_t body_size)
{
	return vmw_cmd_one_flags(v, id, body, body_size, 0);
}

int vmw_cmd_one_sync(struct vmw_device *v, uint32_t id, const void *body,
		     uint32_t body_size)
{
	return vmw_cmd_one_flags(v, id, body, body_size, 1);
}

/* Fire-and-forget: for the per-frame paths (screen-target updates, image
 * updates) where waiting for the device on every rectangle is what made
 * the desktop crawl.  Errors surface in the log when the slot is reaped. */
int vmw_cmd_one_async(struct vmw_device *v, uint32_t id, const void *body,
		      uint32_t body_size)
{
	return vmw_cmd_one_flags(v, id, body, body_size, 0);
}

/* ---- MOBs ------------------------------------------------------------- */

int vmw_mob_alloc_id(struct vmw_device *v)
{
	uint64_t fl;
	spin_lock_irqsave(&v->id_lock, &fl);
	int id = vmw_id_alloc(v->mob_ids, VMW_NUM_MOBS);
	spin_unlock_irqrestore(&v->id_lock, fl);
	return id;
}

void vmw_mob_free_id(struct vmw_device *v, uint32_t id)
{
	uint64_t fl;
	spin_lock_irqsave(&v->id_lock, &fl);
	vmw_id_free(v->mob_ids, id);
	spin_unlock_irqrestore(&v->id_lock, fl);
}

/* Page tables: PT64_0 = one page (base is its PPN); PT64_1 = a page of
 * PPN64 entries (512 pages, 2 MB); PT64_2 = a page of PPN64s of such
 * pages (1 GB). */
int vmw_mob_bind(struct vmw_device *v, struct vmw_mob *mob, const uint64_t *pages,
		 uint32_t npages, uint32_t size_bytes)
{
	uint32_t per_page = PAGE_SIZE / sizeof(uint64_t); /* 512 */

	mob->size = size_bytes;
	mob->pt_pages = NULL;
	mob->npt = 0;
	if (npages == 1) {
		mob->fmt = SVGA3D_MOBFMT_PT64_0;
		mob->base_ppn = pages[0] >> 12;
	} else if (npages <= per_page) {
		mob->fmt = SVGA3D_MOBFMT_PT64_1;
		mob->pt_pages = kalloc(sizeof(uint64_t));
		if (!mob->pt_pages)
			return -ENOMEM;
		mob->pt_pages[0] = mm_allocate_physical_page();
		if (!mob->pt_pages[0]) {
			kfree(mob->pt_pages);
			mob->pt_pages = NULL;
			return -ENOMEM;
		}
		mob->npt = 1;
		uint64_t *pt = phys_to_virt(mob->pt_pages[0]);
		/* Zero first: a fresh physical page holds whatever its last
		 * owner left, and every entry past the end of this MOB would
		 * otherwise be a page number the device might follow. */
		mm_memset(pt, 0, PAGE_SIZE);
		for (uint32_t i = 0; i < npages; i++)
			pt[i] = pages[i] >> 12;
		mob->base_ppn = mob->pt_pages[0] >> 12;
	} else {
		uint32_t nl1 = (npages + per_page - 1) / per_page;
		if (nl1 > per_page)
			return -E2BIG;
		mob->fmt = SVGA3D_MOBFMT_PT64_2;
		mob->pt_pages = kalloc((nl1 + 1) * sizeof(uint64_t));
		if (!mob->pt_pages)
			return -ENOMEM;
		for (uint32_t i = 0; i < nl1 + 1; i++) {
			mob->pt_pages[i] = mm_allocate_physical_page();
			if (!mob->pt_pages[i]) {
				for (uint32_t j = 0; j < i; j++)
					mm_free_physical_page(mob->pt_pages[j]);
				kfree(mob->pt_pages);
				mob->pt_pages = NULL;
				return -ENOMEM;
			}
		}
		mob->npt = nl1 + 1;
		uint64_t *l2 = phys_to_virt(mob->pt_pages[0]);
		mm_memset(l2, 0, PAGE_SIZE);
		for (uint32_t i = 0; i < nl1; i++) {
			l2[i] = mob->pt_pages[1 + i] >> 12;
			uint64_t *l1 = phys_to_virt(mob->pt_pages[1 + i]);
			mm_memset(l1, 0, PAGE_SIZE);
			for (uint32_t j = 0; j < per_page && i * per_page + j < npages; j++)
				l1[j] = pages[i * per_page + j] >> 12;
		}
		mob->base_ppn = mob->pt_pages[0] >> 12;
	}
	SVGA3dCmdDefineGBMob64 c;
	c.mobid = mob->id;
	c.ptDepth = mob->fmt;
	c.base = mob->base_ppn;
	c.sizeInBytes = size_bytes;
	int rc = vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_MOB64, &c, sizeof(c));
	if (rc == 0)
		mob->defined = 1;
	return rc;
}

void vmw_mob_unbind(struct vmw_device *v, struct vmw_mob *mob)
{
	if (mob->defined) {
		SVGA3dCmdDestroyGBMob c;
		c.mobid = mob->id;
		/* Queued like the rest.  What made this one wait was that
		 * its page tables are freed below and the device may still
		 * be walking them -- so the pages are held instead of freed,
		 * and handed back once the device has worked through
		 * everything it was given (defer_free_page). */
		vmw_cmd_one(v, SVGA_3D_CMD_DESTROY_GB_MOB, &c, sizeof(c));
		mob->defined = 0;
	}
	if (mob->pt_pages) {
		for (uint32_t i = 0; i < mob->npt; i++)
			defer_free_page(v, mob->pt_pages[i]);
		kfree(mob->pt_pages);
		mob->pt_pages = NULL;
		mob->npt = 0;
	}
}

/* ---- OTables ------------------------------------------------------------ */

static const uint32_t otable_entry_size[SVGA_OTABLE_DX_MAX] = {
	[SVGA_OTABLE_MOB] = sizeof(SVGAOTableMobEntry),
	[SVGA_OTABLE_SURFACE] = sizeof(SVGAOTableSurfaceEntry),
	[SVGA_OTABLE_CONTEXT] = sizeof(SVGAOTableContextEntry),
	[SVGA_OTABLE_SHADER] = sizeof(SVGAOTableShaderEntry),
	[SVGA_OTABLE_SCREENTARGET] = sizeof(SVGAOTableScreenTargetEntry),
	[SVGA_OTABLE_DXCONTEXT] = sizeof(SVGAOTableDXContextEntry),
};
static const uint32_t otable_entries[SVGA_OTABLE_DX_MAX] = {
	[SVGA_OTABLE_MOB] = VMW_NUM_MOBS,
	[SVGA_OTABLE_SURFACE] = VMW_NUM_SURFACES,
	[SVGA_OTABLE_CONTEXT] = VMW_NUM_CONTEXTS,
	[SVGA_OTABLE_SHADER] = VMW_NUM_SHADERS,
	[SVGA_OTABLE_SCREENTARGET] = VMW_NUM_SCREENTARGETS,
	[SVGA_OTABLE_DXCONTEXT] = VMW_NUM_DXCONTEXTS,
};

/* An OTable is described to the device like a MOB, but with
 * SET_OTABLE_BASE64: it needs page tables of its own. */
static int otable_setup_one(struct vmw_device *v, int type)
{
	uint32_t size = otable_entry_size[type] * otable_entries[type];
	size = (size + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
	struct drm_gem_object *o = drm_gem_alloc(&v->drm, DRM_GEM_BO, size);
	if (!o)
		return -ENOMEM;
	if (drm_gem_alloc_pages(o)) {
		drm_gem_put(o);
		return -ENOMEM;
	}
	/* Page tables (not a MOB: no id, no DEFINE). */
	struct vmw_mob pt;
	mm_memset(&pt, 0, sizeof(pt));
	pt.id = SVGA3D_INVALID_ID;
	uint32_t per_page = PAGE_SIZE / 8;
	if (o->npages == 1) {
		pt.fmt = SVGA3D_MOBFMT_PT64_0;
		pt.base_ppn = o->pages[0] >> 12;
	} else if (o->npages <= per_page) {
		pt.fmt = SVGA3D_MOBFMT_PT64_1;
		uint64_t pp = mm_allocate_physical_page();
		if (!pp) {
			drm_gem_put(o);
			return -ENOMEM;
		}
		uint64_t *t = phys_to_virt(pp);
		/* Zero first: entries past the table's pages would otherwise
		 * be leftover page numbers the device may follow. */
		mm_memset(t, 0, PAGE_SIZE);
		for (uint32_t i = 0; i < o->npages; i++)
			t[i] = o->pages[i] >> 12;
		pt.base_ppn = pp >> 12;
	} else {
		uint32_t nl1 = (o->npages + per_page - 1) / per_page;
		uint64_t l2p = mm_allocate_physical_page();
		if (!l2p) {
			drm_gem_put(o);
			return -ENOMEM;
		}
		uint64_t *l2 = phys_to_virt(l2p);
		mm_memset(l2, 0, PAGE_SIZE);
		for (uint32_t i = 0; i < nl1; i++) {
			uint64_t l1p = mm_allocate_physical_page();
			if (!l1p) {
				drm_gem_put(o);
				return -ENOMEM;
			}
			l2[i] = l1p >> 12;
			uint64_t *l1 = phys_to_virt(l1p);
			mm_memset(l1, 0, PAGE_SIZE);
			for (uint32_t j = 0; j < per_page && i * per_page + j < o->npages; j++)
				l1[j] = o->pages[i * per_page + j] >> 12;
		}
		pt.fmt = SVGA3D_MOBFMT_PT64_2;
		pt.base_ppn = l2p >> 12;
	}
	SVGA3dCmdSetOTableBase64 c;
	c.type = (SVGAOTableType)type;
	c.baseAddress = pt.base_ppn;
	c.sizeInBytes = size;
	c.validSizeInBytes = 0;
	c.ptDepth = pt.fmt;
	int rc = vmw_cmd_one_sync(v, SVGA_3D_CMD_SET_OTABLE_BASE64, &c, sizeof(c));
	if (rc) {
		drm_gem_put(o);
		return rc;
	}
	v->otable[type].bo = o; /* page tables leak on takedown: tiny, once */
	v->otable[type].size = size;
	return 0;
}

int vmw_otables_setup(struct vmw_device *v)
{
	int last = v->has_dx ? SVGA_OTABLE_DX_MAX : SVGA_OTABLE_DX9_MAX;

	for (int t = 0; t < last; t++) {
		int rc = otable_setup_one(v, t);
		if (rc) {
			kprintf("[drm] vmwgfx: OTable %d setup failed (%d)\n", t, rc);
			return rc;
		}
	}
	v->otables_ready = 1;
	return 0;
}

void vmw_otables_takedown(struct vmw_device *v)
{
	int last = v->has_dx ? SVGA_OTABLE_DX_MAX : SVGA_OTABLE_DX9_MAX;

	for (int t = 0; t < last; t++) {
		if (!v->otable[t].bo)
			continue;
		SVGA3dCmdSetOTableBase64 c;
		c.type = (SVGAOTableType)t;
		c.baseAddress = 0;
		c.sizeInBytes = 0;
		c.validSizeInBytes = 0;
		c.ptDepth = SVGA3D_MOBFMT_INVALID;
		vmw_cmd_one(v, SVGA_3D_CMD_SET_OTABLE_BASE64, &c, sizeof(c));
	}
	v->otables_ready = 0;
}

int vmw_gb_init(struct vmw_device *v)
{
	spinlock_init(&v->id_lock, "vmw_ids");
	spinlock_init(&v->defer_lock, "vmw_defer");
	v->mob_ids = kalloc(VMW_NUM_MOBS / 8);
	v->surface_ids = kalloc(VMW_NUM_SURFACES / 8);
	v->context_ids = kalloc(VMW_NUM_CONTEXTS / 8);
	v->shader_ids = kalloc(VMW_NUM_SHADERS / 8);
	if (!v->mob_ids || !v->surface_ids || !v->context_ids || !v->shader_ids)
		return -ENOMEM;
	mm_memset(v->mob_ids, 0, VMW_NUM_MOBS / 8);
	mm_memset(v->surface_ids, 0, VMW_NUM_SURFACES / 8);
	mm_memset(v->context_ids, 0, VMW_NUM_CONTEXTS / 8);
	mm_memset(v->shader_ids, 0, VMW_NUM_SHADERS / 8);
	/* id 0 of each is left unused: userspace treats 0 as a valid id but
	 * a few consumers assume "0 = none". */
	v->mob_ids[0] |= 1;
	v->surface_ids[0] |= 1;
	v->context_ids[0] |= 1;
	v->shader_ids[0] |= 1;
	int rc = vmw_cmdbuf_bringup(v);
	if (rc)
		return rc;
	if (!v->has_gb)
		return 0;
	return vmw_otables_setup(v);
}
