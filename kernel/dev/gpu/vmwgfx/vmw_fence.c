// LikeOS -- vmwgfx: fences, the fence manager and fence events.
//
// When the device has passed a fence: from the command-buffer channel's
// completions while that is up, from the FIFO's fence register otherwise.
// Checked from the device interrupt, from a short poll timer while fences
// are outstanding (a host without the fence interrupt still gets them
// signalled), and by every waiter before it waits.
//
// The manager keeps every fence this driver emitted and has not yet seen
// pass on one list, in sequence order, each with the actions to run when it
// passes -- a DRM event a client asked for with DRM_VMW_FENCE_EVENT is one.
// A sweep retires the passed fences from the front, runs their actions, and
// moves the device's fence goal on to the next fence somebody is waiting
// for.  The fence interrupts are armed through the console driver's waiter
// counts only while something here needs them: the any-fence interrupt
// while fences are pending on the FIFO channel, the goal interrupt while a
// goal is programmed.  On the command-buffer channel neither is needed: a
// buffer's completion is announced by its own interrupt, which is always on.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's and VMware's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright 2011-2012 VMware, Inc., Palo Alto, CA., USA
// Portions Copyright 2009-2015 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/ioctl.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>

/* 1: program the device's fence goal for EVERY fence emitted, on either
 * channel, as this driver did before the manager kept its own list (the
 * console driver then holds the fence interrupts until the goal passes).
 * 0: the manager arms what it needs through the waiter counts -- see the
 * top of this file.  A way back, should a host turn out to depend on the
 * per-fence goal. */
#ifndef VMW_FENCE_IRQ_LEGACY_GOAL
#define VMW_FENCE_IRQ_LEGACY_GOAL 0
#endif

/* 1: on the FIFO channel, hold the any-fence interrupt for as long as any
 * emitted fence is pending, so that every fence is noticed the moment it
 * passes -- including by a poll() on a sync_file or a GEM wait, which the
 * driver cannot see and which otherwise only the 2 ms poll timer would
 * serve.  This keeps the interrupt rate the per-fence goal used to give.
 * 0: arm it only around an explicit wait (vmw_fence_obj_wait()), the goal
 * interrupt for fences with actions, and the poll timer for the rest. */
#ifndef VMW_FENCE_SEQNO_IRQ_WHILE_PENDING
#define VMW_FENCE_SEQNO_IRQ_WHILE_PENDING 1
#endif

/* A sequence counts as passed when the device's last-passed value is at
 * most this far beyond it. */
#define VMW_FENCE_WRAP (1U << 31)

/* The poll timer's period while fences are outstanding. */
#define VMW_FENCE_POLL_NS 2000000ULL

/* How many times one check goes back to the device because the sweep moved
 * the goal or unmasked an interrupt (either can hide a fence that passed in
 * between); more than a couple means the device is moving fast enough that
 * the next interrupt will do. */
#define VMW_FENCE_UPDATE_RERUNS 4

/* Events one file may have waiting for fences: what a page of event space
 * holds, the bound a client is held to before it has read any of them.
 * And a bound on all actions together, so that no number of files can pin
 * unbounded memory. */
#define VMW_FENCE_EVENTS_PER_FILE (4096 / sizeof(struct drm_vmw_event_fence))
#define VMW_FENCE_ACTIONS_MAX 4096

enum vmw_action_type {
	VMW_ACTION_EVENT = 0,
	VMW_ACTION_MAX,
};

/* Something to do when a fence passes. */
struct vmw_fence_action {
	struct list_head head;
	enum vmw_action_type type;
	/* The file the action works for, or NULL.  vmw_fence_file_release()
	 * drops a closing file's actions before it is freed. */
	struct drm_file *fp;
	/* The fence has passed.  Called with the manager's lock held and
	 * interrupts off -- from the interrupt handler and the poll timer as
	 * well as from tasks -- so it must not sleep. */
	void (*seq_passed)(struct vmw_fence_action *action, struct drm_fence *f);
	/* Then, with the lock dropped: free the action. */
	void (*cleanup)(struct vmw_fence_action *action);
};

/* A DRM event to queue when the fence passes. */
struct vmw_event_fence_action {
	struct vmw_fence_action action;
	/* Where in `event' the signal time goes (u32 seconds, u32
	 * microseconds), or -1 for no time. */
	int32_t tv_sec_off, tv_usec_off;
	uint32_t length;
	uint8_t event[64];
};

/* The manager's record of one pending fence. */
struct vmw_fence_obj {
	struct list_head head; /* fman->fence_list */
	/* Referenced for as long as the record is on the list: the list is
	 * how a passed fence's actions are found, so the fence must outlive
	 * the last of its own users until then. */
	struct drm_fence *fence;
	uint32_t seqno;
	/* On the device-wide sequence (kept in order by it).  A record for
	 * any other fence sits at the end, out of the ordering, and is
	 * retired by its flag alone. */
	int ordered;
	struct list_head seq_passed_actions;
};

struct vmw_fence_manager {
	struct vmw_device *v;
	/* Taken from an interrupt handler -- vmw_irq_cb() -> vmw_fence_check()
	 * -> the sweep -- and from the poll timer, so EVERY acquisition of it
	 * disables interrupts first, process-context ones included.
	 *
	 * With a plain spin_lock() on the fence-event table that this list
	 * replaced, the DRM_VMW_FENCE_WAIT ioctl (which runs the check itself,
	 * in process context with interrupts on) held the lock while the SVGA
	 * interrupt arrived on the same processor, and the handler then spun
	 * for that same lock -- against a holder that could not make progress
	 * until the handler returned.  A hard hang of that CPU, at 500 million
	 * spins per report, with the interrupted ioctl frame still on the
	 * stack underneath the handler's.
	 *
	 * In an interrupt handler the save/restore costs nothing (IF is
	 * already clear); the guarantee it buys in process context is the
	 * whole point.
	 *
	 * Nests outside the DRM file's lock (events are queued under it) and
	 * outside the console driver's interrupt-mask lock (the waiter counts
	 * are changed under it); never inside either. */
	spinlock_t lock;
	struct list_head fence_list; /* pending fences, oldest first */
	uint32_t num_fence_objs;
	uint32_t pending_actions[VMW_ACTION_MAX];
	uint32_t num_actions;
	/* Emission is down (vmw_fence_fifo_down()): new fences are not
	 * tracked. */
	int fifo_down;
	/* Taken down: the poll timer is not re-armed. */
	int stopping;
	/* The fence goal: `seqno_valid' while `goal_seqno' is programmed for
	 * a pending fence with actions. */
	bool seqno_valid;
	uint32_t goal_seqno;
	/* The waiter counts this manager holds in the console driver. */
	bool goal_irq_on;
	bool seqno_irq_on;
};

/* One device; neither the interrupt nor the DRM core's poll hook carries
 * it, and the manager lives as long as the kernel does. */
static struct vmw_device *g_fence_dev;
static struct vmw_fence_manager g_fman;

static inline bool vmw_seq_passed(uint32_t passed, uint32_t seq)
{
	return passed - seq < VMW_FENCE_WRAP;
}

/* The fences are on the FIFO channel -- the device's fence register is
 * their truth -- and the fence interrupts can be armed. */
static bool vmw_fence_irq_fifo(struct vmw_device *v)
{
	return !v->cb_ready && v->hw.irq_enabled && vmsvga2_hw_has_fence();
}

static bool vmw_fence_goal_usable(struct vmw_device *v)
{
	return vmw_fence_irq_fifo(v) &&
	       vmsvga2_hw_has_fifo_reg(SVGA_FIFO_FENCE_GOAL);
}

/* ---- the device's view ------------------------------------------------- */

/* Signal every drm_fence the device has passed. */
static void vmw_fence_signal_from_device(struct vmw_device *v)
{
	if (v->cb_ready) {
		/* Collecting the finished command buffers IS the fence check.
		 *
		 * A fence emitted through the command-buffer channel is a
		 * command inside one of those buffers, and it has passed when
		 * the buffer completes -- which is what cb_slot_completed()
		 * reports.  The device's fence REGISTER must not be consulted
		 * here: it belongs to the FIFO, and the console writes
		 * SVGA_CMD_FENCE into the FIFO out of the same counter these
		 * numbers come from.  The FIFO drains on its own, so that
		 * register runs ahead of anything a command-buffer context has
		 * executed, and reading it as "fences up to here have passed"
		 * signalled every client fence the moment it was created.
		 *
		 * Nothing failed when it did.  Every wait returned at once, so
		 * a client mapped a surface before the device had written it
		 * and read the frame before -- rendering that had plainly
		 * worked, arriving one frame late for ever.  Under X that is
		 * text and window content one repaint behind, which mostly
		 * means blank.
		 *
		 * For the same reason the fence goal and the any-fence
		 * interrupt -- both about that register -- are never armed for
		 * this channel; see vmw_fence_irq_fifo(). */
		vmw_cmdbuf_poll(v);
	} else {
		/* No command buffers: the work IS the FIFO, and the FIFO's
		 * fence register is exactly the right answer. */
		uint32_t cur = vmsvga2_hw_fence_current();

		if (cur)
			drm_fence_signal_upto(&v->drm, cur);
	}
}

/* ---- the goal and the interrupt holds ---------------------------------- */

/* Caller holds fman->lock. */
static void vmw_fence_goal_write_locked(struct vmw_fence_manager *fman,
					uint32_t seq)
{
	fman->goal_seqno = seq;
	fman->seqno_valid = true;
	/* The console driver owns the register and will not pull a goal it
	 * still has outstanding back to an earlier fence; while that goal is
	 * outstanding it holds the any-fence interrupt, so the earlier fence
	 * is noticed all the same. */
	vmsvga2_hw_set_fence_goal(seq);
}

/* The goal has been passed: program the next pending fence with actions,
 * or nothing.  True when the goal moved -- the device may have passed the
 * new one before it was written, and no interrupt would say so.  Caller
 * holds fman->lock. */
static bool vmw_fence_goal_new_locked(struct vmw_fence_manager *fman,
				      uint32_t passed)
{
	struct vmw_fence_obj *obj;

	if (!fman->seqno_valid)
		return false;
	if (!vmw_fence_goal_usable(fman->v)) {
		/* The channel changed under the goal: nothing will report it. */
		fman->seqno_valid = false;
		return false;
	}
	if (!vmw_seq_passed(passed, fman->goal_seqno))
		return false;

	fman->seqno_valid = false;
	list_for_each_entry(obj, &fman->fence_list, head) {
		if (obj->ordered && !obj->fence->signaled &&
		    !list_empty(&obj->seq_passed_actions)) {
			vmw_fence_goal_write_locked(fman, obj->seqno);
			break;
		}
	}
	return true;
}

/* An action was queued on `obj': make sure an interrupt comes when it
 * passes.  The goal is the OLDEST pending fence with actions, so the first
 * interrupt arrives with the first action due; a goal already at or before
 * `obj' stays, and is moved on by vmw_fence_goal_new_locked() once passed.
 * True when the goal was written (look at the device again).  Caller holds
 * fman->lock. */
static bool vmw_fence_goal_check_locked(struct vmw_fence_manager *fman,
					struct vmw_fence_obj *obj)
{
	if (!vmw_fence_goal_usable(fman->v) || !obj->ordered ||
	    obj->fence->signaled)
		return false;
	if (fman->seqno_valid && vmw_seq_passed(obj->seqno, fman->goal_seqno))
		return false;
	vmw_fence_goal_write_locked(fman, obj->seqno);
	return true;
}

/* Bring the waiter counts this manager holds in line with what it needs.
 * True when a source was newly unmasked: a status that latched while it
 * was masked has been acknowledged, so the condition must be looked at
 * again.  Caller holds fman->lock. */
static bool vmw_fence_irq_reconcile_locked(struct vmw_fence_manager *fman)
{
	struct vmw_device *v = fman->v;
	bool want_goal = fman->seqno_valid && !fman->stopping;
	bool want_seqno = VMW_FENCE_SEQNO_IRQ_WHILE_PENDING &&
			  !fman->stopping && vmw_fence_irq_fifo(v) &&
			  fman->num_fence_objs > 0;
	bool rerun = false;

	if (want_goal != fman->goal_irq_on) {
		fman->goal_irq_on = want_goal;
		if (want_goal)
			rerun |= vmsvga2_irq_goal_waiter_add();
		else
			(void)vmsvga2_irq_goal_waiter_remove();
	}
	if (want_seqno != fman->seqno_irq_on) {
		fman->seqno_irq_on = want_seqno;
		if (want_seqno)
			rerun |= vmsvga2_irq_seqno_waiter_add();
		else
			(void)vmsvga2_irq_seqno_waiter_remove();
	}
	return rerun;
}

/* ---- the pending list --------------------------------------------------- */

/* Caller holds fman->lock. */
static struct vmw_fence_obj *vmw_fence_obj_find_locked(struct vmw_fence_manager *fman,
						       struct drm_fence *f)
{
	struct vmw_fence_obj *obj;

	/* Newest first: the fence an action names is nearly always recent. */
	list_for_each_entry_reverse(obj, &fman->fence_list, head)
		if (obj->fence == f)
			return obj;
	return NULL;
}

/* Fill in a record for `f' (taking a reference) and put it in sequence
 * order.  Caller holds fman->lock. */
static void vmw_fence_obj_insert_locked(struct vmw_fence_manager *fman,
					struct vmw_fence_obj *obj,
					struct drm_fence *f)
{
	struct vmw_fence_obj *pos;

	drm_fence_get(f);
	obj->fence = f;
	obj->seqno = f->seqno;
	obj->ordered = !f->context && !f->deps[0];
	INIT_LIST_HEAD(&obj->seq_passed_actions);
	fman->num_fence_objs++;

	if (!obj->ordered) {
		list_add_tail(&obj->head, &fman->fence_list);
		return;
	}
	/* From the back: fences are emitted in sequence order, so this is
	 * nearly always the first comparison.  Two emitters can still swap
	 * between taking a number and getting here. */
	list_for_each_entry_reverse(pos, &fman->fence_list, head) {
		if (!pos->ordered)
			continue;
		if ((int32_t)(obj->seqno - pos->seqno) >= 0) {
			list_add(&obj->head, &pos->head);
			return;
		}
	}
	list_add(&obj->head, &fman->fence_list);
}

/* Run (under the lock) and collect (for cleanup after it) every action on
 * `actions'.  Caller holds fman->lock. */
static void vmw_fences_perform_actions_locked(struct vmw_fence_manager *fman,
					      struct drm_fence *f,
					      struct list_head *actions,
					      struct list_head *cleanup)
{
	struct vmw_fence_action *action, *next;

	list_for_each_entry_safe(action, next, actions, head) {
		list_del(&action->head);
		fman->pending_actions[action->type]--;
		fman->num_actions--;
		if (action->seq_passed)
			action->seq_passed(action, f);
		list_add_tail(&action->head, cleanup);
	}
}

static void vmw_fence_actions_cleanup(struct list_head *cleanup)
{
	struct vmw_fence_action *action, *next;

	list_for_each_entry_safe(action, next, cleanup, head) {
		list_del(&action->head);
		if (action->cleanup)
			action->cleanup(action);
	}
}

static void vmw_fence_objs_free(struct list_head *done)
{
	struct vmw_fence_obj *obj, *next;

	list_for_each_entry_safe(obj, next, done, head) {
		list_del(&obj->head);
		drm_fence_put(obj->fence);
		kfree(obj);
	}
}

/* Retire every pending fence that has signalled and run its actions, then
 * move the goal and the interrupt holds.  True when the device should be
 * looked at again (see VMW_FENCE_UPDATE_RERUNS). */
static bool __vmw_fences_update(struct vmw_device *v)
{
	struct vmw_fence_manager *fman = v->fman;
	struct vmw_fence_obj *obj, *next;
	LIST_HEAD(done);
	LIST_HEAD(cleanup);
	uint64_t fl;
	bool rerun;

	if (!fman)
		return false;
	/* Nothing pending and nothing armed: the common case of a check
	 * from the spinning phase of a wait on an idle device.  A racing
	 * insert reconciles the holds itself. */
	if (list_empty(&fman->fence_list) && !fman->seqno_valid &&
	    !fman->goal_irq_on && !fman->seqno_irq_on)
		return false;

	spin_lock_irqsave(&fman->lock, &fl);
	/* By the flag, not by comparing sequences: drm_fence_signal_upto()
	 * publishes the new passed value before it has marked every fence
	 * it covers, so a fence at or below it can still be unmarked for a
	 * moment -- and actions are owed only once the fence is signalled
	 * (an event carries its signal time).  The whole list is walked; it
	 * holds what is in flight, a handful. */
	list_for_each_entry_safe(obj, next, &fman->fence_list, head) {
		if (!obj->fence->signaled)
			continue;
		list_del(&obj->head);
		fman->num_fence_objs--;
		vmw_fences_perform_actions_locked(fman, obj->fence,
						  &obj->seq_passed_actions,
						  &cleanup);
		list_add_tail(&obj->head, &done);
	}
	rerun = vmw_fence_goal_new_locked(fman, v->drm.fence_passed);
	rerun |= vmw_fence_irq_reconcile_locked(fman);
	spin_unlock_irqrestore(&fman->lock, fl);

	vmw_fence_actions_cleanup(&cleanup);
	vmw_fence_objs_free(&done);
	return rerun;
}

/* ---- checking ------------------------------------------------------------ */

void vmw_fence_check(struct vmw_device *v)
{
	if (!v)
		return;
	for (int pass = 0; pass < VMW_FENCE_UPDATE_RERUNS; pass++) {
		vmw_fence_signal_from_device(v);
		if (!__vmw_fences_update(v))
			break;
	}
}

uint32_t vmw_fences_update(struct vmw_device *v)
{
	vmw_fence_check(v);
	return v->drm.fence_passed;
}

bool vmw_fence_obj_signaled(struct vmw_device *v, struct drm_fence *f)
{
	if (f->signaled)
		return true;
	vmw_fence_check(v);
	return f->signaled != 0;
}

/* ---- the poll timer ------------------------------------------------------ */

static void vmw_fence_poll_fire(hrtimer_t *t)
{
	struct vmw_device *v = t->arg;
	struct vmw_fence_manager *fman = v->fman;
	uint64_t fl;

	vmw_fence_check(v);
	/* Decided under the lock that the arming side takes too: deciding
	 * "nothing left" here while a fence is being emitted on another
	 * processor, which saw the timer still running and so did not start
	 * it, used to leave that fence to the interrupt alone. */
	spin_lock_irqsave(&fman->lock, &fl);
	if (!fman->stopping &&
	    (v->drm.fences || !list_empty(&fman->fence_list)))
		hrtimer_start_rel(&v->fence_poll, VMW_FENCE_POLL_NS);
	else
		v->fence_poll_running = 0;
	spin_unlock_irqrestore(&fman->lock, fl);
}

static void vmw_fence_poll_arm(struct vmw_device *v)
{
	struct vmw_fence_manager *fman = v->fman;
	uint64_t fl;

	spin_lock_irqsave(&fman->lock, &fl);
	if (!v->fence_poll_running && !fman->stopping) {
		v->fence_poll_running = 1;
		hrtimer_start_rel(&v->fence_poll, VMW_FENCE_POLL_NS);
	}
	spin_unlock_irqrestore(&fman->lock, fl);
}

void vmw_irq_cb(uint32_t status)
{
	/* Every interrupt the device raised, whatever the bits: the
	 * command-buffer completions are collected from here too. */
	(void)status;
	vmw_fence_check(g_fence_dev);
}

void vmw_drm_fence_poll(struct drm_device *dev)
{
	(void)dev;
	vmw_fence_check(g_fence_dev);
}

/* ---- emission ------------------------------------------------------------ */

/* Put a freshly emitted, unsignalled fence on the pending list.  Without
 * memory for the record the fence is still signalled by the device sweep
 * like any other; an action queued on it later makes the record then. */
static void vmw_fence_track(struct vmw_device *v, struct drm_fence *f)
{
	struct vmw_fence_manager *fman = v->fman;
	struct vmw_fence_obj *obj = kalloc(sizeof(*obj));
	bool rerun;
	uint64_t fl;

	if (!obj)
		return;
	mm_memset(obj, 0, sizeof(*obj));
	spin_lock_irqsave(&fman->lock, &fl);
	if (fman->fifo_down) {
		spin_unlock_irqrestore(&fman->lock, fl);
		kfree(obj);
		return;
	}
	vmw_fence_obj_insert_locked(fman, obj, f);
	rerun = vmw_fence_irq_reconcile_locked(fman);
	spin_unlock_irqrestore(&fman->lock, fl);
	/* The any-fence interrupt was just unmasked: the fence may have gone
	 * by while it was not. */
	if (rerun)
		vmw_fence_check(v);
}

/* Insert a fence after whatever was just submitted; returns it. */
struct drm_fence *vmw_fence_emit(struct vmw_device *v, uint32_t flags)
{
	uint32_t seq;

	/* Down the channel the work went.  With command buffers carrying the
	 * commands, a fence written to the FIFO is in a different queue from
	 * the batches it is supposed to follow and can pass while they are
	 * still running -- so the fence would promise completion that has
	 * not happened.  Command buffers take FIFO-format commands, and one
	 * context executes strictly in submission order. */
	if (v->cb_ready) {
		/* Allocated and submitted together: see vmw_cmd_fence_emit().
		 * Handing out the number first and submitting afterwards lets
		 * two threads swap, and then the higher number completes first
		 * and signals the lower -- whose work has not run. */
		seq = vmw_cmd_fence_emit(v);
	} else {
		seq = vmsvga2_fence_insert();
	}

	if (!seq) {
		/* No fence support (QEMU): everything completes in order
		 * with the doorbell, so a signalled fence is the truth. */
		vmsvga2_fifo_flush();
		return drm_fence_signalled(&v->drm);
	}
	v->drm.fence_seq = seq;
	struct drm_fence *f = drm_fence_create(&v->drm, seq, flags);
	if (f && !f->signaled) {
		if (v->fman)
			vmw_fence_track(v, f);
#if VMW_FENCE_IRQ_LEGACY_GOAL
		if (v->hw.irq_enabled)
			vmsvga2_hw_set_fence_goal(seq);
#endif
		if (v->fman)
			vmw_fence_poll_arm(v);
	}
	return f;
}

/* ---- actions --------------------------------------------------------------- */

/* Pending actions of `fp'.  Caller holds fman->lock. */
static uint32_t vmw_fence_file_actions_locked(struct vmw_fence_manager *fman,
					      struct drm_file *fp)
{
	struct vmw_fence_obj *obj;
	struct vmw_fence_action *action;
	uint32_t n = 0;

	list_for_each_entry(obj, &fman->fence_list, head)
		list_for_each_entry(action, &obj->seq_passed_actions, head)
			if (action->fp == fp)
				n++;
	return n;
}

/* Run `action' when `f' passes -- now, if it already has.  On success the
 * action belongs to the manager (its cleanup frees it); on failure the
 * caller still owns it. */
static int vmw_fence_obj_add_action(struct vmw_device *v, struct drm_fence *f,
				    struct vmw_fence_action *action)
{
	struct vmw_fence_manager *fman = v->fman;
	struct vmw_fence_obj *spare = NULL, *obj;
	LIST_HEAD(now);
	LIST_HEAD(cleanup);
	bool pending = false;
	uint64_t fl;

	if (!fman || f->dev != &v->drm)
		return -EINVAL;
	/* A record for a fence the list does not have yet, made before the
	 * lock: nothing allocates under it. */
	if (!f->signaled) {
		spare = kalloc(sizeof(*spare));
		if (!spare)
			return -ENOMEM;
		mm_memset(spare, 0, sizeof(*spare));
	}

	spin_lock_irqsave(&fman->lock, &fl);
	if (fman->num_actions >= VMW_FENCE_ACTIONS_MAX ||
	    (action->type == VMW_ACTION_EVENT && action->fp &&
	     vmw_fence_file_actions_locked(fman, action->fp) >=
		     VMW_FENCE_EVENTS_PER_FILE)) {
		spin_unlock_irqrestore(&fman->lock, fl);
		if (spare)
			kfree(spare);
		return -ENOMEM;
	}
	fman->pending_actions[action->type]++;
	fman->num_actions++;
	if (f->signaled) {
		list_add_tail(&action->head, &now);
		vmw_fences_perform_actions_locked(fman, f, &now, &cleanup);
	} else {
		obj = vmw_fence_obj_find_locked(fman, f);
		if (!obj) {
			obj = spare;
			spare = NULL;
			vmw_fence_obj_insert_locked(fman, obj, f);
		}
		list_add_tail(&action->head, &obj->seq_passed_actions);
		(void)vmw_fence_goal_check_locked(fman, obj);
		(void)vmw_fence_irq_reconcile_locked(fman);
		pending = true;
	}
	spin_unlock_irqrestore(&fman->lock, fl);

	if (spare)
		kfree(spare);
	vmw_fence_actions_cleanup(&cleanup);
	if (pending) {
		/* With the goal written and the interrupts armed, look once
		 * more: the fence may have passed in between, and nothing
		 * would report it then. */
		vmw_fence_check(v);
		vmw_fence_poll_arm(v);
	}
	return 0;
}

static void vmw_event_fence_action_seq_passed(struct vmw_fence_action *action,
					      struct drm_fence *f)
{
	struct vmw_event_fence_action *ea =
		container_of(action, struct vmw_event_fence_action, action);

	if (ea->tv_sec_off >= 0) {
		/* The monotonic clock the fence was signalled on: no wrap of
		 * the seconds for a long while. */
		uint64_t ns = f->signal_ns;
		uint32_t sec = (uint32_t)(ns / 1000000000ULL);
		uint32_t usec = (uint32_t)((ns % 1000000000ULL) / 1000ULL);

		mm_memcpy(ea->event + ea->tv_sec_off, &sec, sizeof(sec));
		mm_memcpy(ea->event + ea->tv_usec_off, &usec, sizeof(usec));
	}
	/* Under the manager's lock, which vmw_fence_file_release() takes
	 * too: the file cannot be freed while its event is being queued. */
	drm_event_queue(action->fp, ea->event, ea->length);
}

static void vmw_event_fence_action_cleanup(struct vmw_fence_action *action)
{
	kfree(container_of(action, struct vmw_event_fence_action, action));
}

int vmw_event_fence_action_queue(struct vmw_device *v, struct drm_file *fp,
				 struct drm_fence *f, const void *event,
				 uint32_t length, int32_t tv_sec_off,
				 int32_t tv_usec_off)
{
	struct vmw_event_fence_action *ea;
	int ret;

	if (!fp || !f || !event || length < sizeof(struct drm_event) ||
	    length > sizeof(ea->event))
		return -EINVAL;
	if (tv_sec_off >= 0 &&
	    ((uint32_t)tv_sec_off + sizeof(uint32_t) > length ||
	     tv_usec_off < 0 ||
	     (uint32_t)tv_usec_off + sizeof(uint32_t) > length))
		return -EINVAL;

	ea = kalloc(sizeof(*ea));
	if (!ea)
		return -ENOMEM;
	mm_memset(ea, 0, sizeof(*ea));
	ea->action.type = VMW_ACTION_EVENT;
	ea->action.fp = fp;
	ea->action.seq_passed = vmw_event_fence_action_seq_passed;
	ea->action.cleanup = vmw_event_fence_action_cleanup;
	ea->tv_sec_off = tv_sec_off >= 0 ? tv_sec_off : -1;
	ea->tv_usec_off = tv_sec_off >= 0 ? tv_usec_off : -1;
	ea->length = length;
	mm_memcpy(ea->event, event, length);

	ret = vmw_fence_obj_add_action(v, f, &ea->action);
	if (ret)
		kfree(ea);
	return ret;
}

static int vmw_event_fence_action_create(struct vmw_device *v,
					 struct drm_file *fp,
					 struct drm_fence *f, uint32_t flags,
					 uint64_t user_data)
{
	struct drm_vmw_event_fence ev;

	mm_memset(&ev, 0, sizeof(ev));
	ev.base.type = DRM_VMW_EVENT_FENCE_SIGNALED;
	ev.base.length = sizeof(ev);
	ev.user_data = user_data;
	if (flags & DRM_VMW_FE_FLAG_REQ_TIME)
		return vmw_event_fence_action_queue(
			v, fp, f, &ev, sizeof(ev),
			(int32_t)offsetof(struct drm_vmw_event_fence, tv_sec),
			(int32_t)offsetof(struct drm_vmw_event_fence, tv_usec));
	return vmw_event_fence_action_queue(v, fp, f, &ev, sizeof(ev), -1, -1);
}

void vmw_fence_file_release(struct vmw_device *v, struct drm_file *fp)
{
	struct vmw_fence_manager *fman = v ? v->fman : NULL;
	struct vmw_fence_obj *obj;
	struct vmw_fence_action *action, *next;
	LIST_HEAD(cleanup);
	uint64_t fl;

	if (!fman)
		return;
	/* The fences stay pending (others may wait for them); only what was
	 * to be sent to this file goes.  A goal programmed for one of them
	 * passes in its own time and moves on to whatever is left. */
	spin_lock_irqsave(&fman->lock, &fl);
	list_for_each_entry(obj, &fman->fence_list, head) {
		list_for_each_entry_safe(action, next, &obj->seq_passed_actions,
					 head) {
			if (action->fp != fp)
				continue;
			list_del(&action->head);
			fman->pending_actions[action->type]--;
			fman->num_actions--;
			list_add_tail(&action->head, &cleanup);
		}
	}
	spin_unlock_irqrestore(&fman->lock, fl);
	vmw_fence_actions_cleanup(&cleanup);
}

/* ---- waiting ----------------------------------------------------------------- */

int vmw_fence_obj_wait(struct vmw_device *v, struct drm_fence *f, bool lazy,
		       bool interruptible, uint64_t timeout_ns)
{
	bool waiter = false;
	int ret;

	/* Sleeping lightly or not is the waiting code's business here: it
	 * spins briefly, then sleeps with a short deadline, in either case. */
	(void)lazy;
	if (f->signaled)
		return 0;
	/* On the FIFO channel nothing may be armed for this fence (with
	 * VMW_FENCE_SEQNO_IRQ_WHILE_PENDING off, or for a fence that is not
	 * on the list): hold the any-fence interrupt for the wait.  On the
	 * command-buffer channel the completion interrupt is always on. */
	if (vmw_fence_irq_fifo(v)) {
		waiter = true;
		(void)vmsvga2_irq_seqno_waiter_add();
	}
	/* Look after arming: a fence that went by while the source was
	 * masked raised nothing anyone will see. */
	vmw_fence_check(v);
	ret = drm_fence_wait_flags(f, timeout_ns, interruptible ? 1 : 0);
	if (waiter)
		(void)vmsvga2_irq_seqno_waiter_remove();
	if (ret == -ETIMEDOUT)
		ret = -EBUSY;
	return ret;
}

int vmw_fence_wait(struct vmw_device *v, struct drm_fence *f,
		   uint64_t timeout_ns)
{
	int ret = vmw_fence_obj_wait(v, f, false, true, timeout_ns);

	return ret == -EBUSY ? -ETIMEDOUT : ret;
}

/* ---- user space ---------------------------------------------------------------- */

void vmw_fence_copy_user_rep(struct vmw_device *v, struct drm_file *fp,
			     int ret, uint64_t user_rep, struct drm_fence *f,
			     uint32_t handle, int32_t out_fence_fd)
{
	struct drm_vmw_fence_rep rep;

	if (!user_rep)
		return;
	mm_memset(&rep, 0, sizeof(rep));
	rep.error = ret;
	rep.fd = out_fence_fd;
	if (ret == 0 && f) {
		rep.handle = handle;
		/* `mask' stays ZERO -- not the fence's flags.  Mesa's winsys
		 * reads `mask' the other way round: on every wait and every
		 * "has it passed?" it computes EXEC & ~mask and treats a
		 * result of zero as "nothing left to wait for"
		 * (vmw_fence_finish/vmw_fence_signalled).  Reporting EXEC|QUERY
		 * told user space that every fence had passed the moment it was
		 * created: no map ever waited for its readback, and buffers
		 * were reused while the device was still reading them. */
		rep.mask = 0;
		rep.seqno = f->seqno;
		rep.passed_seqno = vmw_fences_update(v);
	}
	/* A failed copy is seen by user space as `error' never having been
	 * filled in (it presets it to -EFAULT).  It has then lost the fence:
	 * drop the handle it will never name, and make the work done before
	 * returning, since it cannot wait for it itself. */
	if (!validate_user_ptr(user_rep, sizeof(rep)) ||
	    copy_to_user((void *)(uintptr_t)user_rep, &rep, sizeof(rep)) != 0) {
		if (rep.error == 0 && f) {
			if (handle)
				(void)drm_fence_handle_delete(fp, handle);
			(void)vmw_fence_obj_wait(v, f, false, false,
						 VMW_FENCE_WAIT_TIMEOUT_NS);
		}
	}
}

long vmw_fence_obj_wait_ioctl(struct vmw_device *v, struct drm_file *fp,
			      struct drm_vmw_fence_wait_arg *a)
{
	uint64_t now = hrtimer_now_ns();
	uint64_t wait_ns;
	struct drm_fence *f;
	long ret;

	/* Microseconds.  The deadline is fixed on the first call and handed
	 * back in the cookie, so a wait restarted after a signal keeps the
	 * deadline it started with rather than beginning a new one. */
	wait_ns = a->timeout_us > UINT64_MAX / 1000ULL ? UINT64_MAX :
							 a->timeout_us * 1000ULL;
	if (!a->cookie_valid) {
		a->cookie_valid = 1;
		a->kernel_cookie = wait_ns > UINT64_MAX - now ? UINT64_MAX :
								now + wait_ns;
	}

	f = drm_fence_handle_lookup(fp, a->handle);
	if (!f)
		return -EINVAL;

	if (now >= a->kernel_cookie)
		ret = vmw_fence_obj_signaled(v, f) ? 0 : -EBUSY;
	else
		ret = vmw_fence_obj_wait(v, f, a->lazy != 0, true,
					 a->kernel_cookie - now);
	drm_fence_put(f);

	/* Optionally drop the handle: only once the fence has passed. */
	if (ret == 0 && (a->wait_options & DRM_VMW_WAIT_OPTION_UNREF))
		return drm_fence_handle_delete(fp, a->handle);
	return ret;
}

long vmw_fence_obj_signaled_ioctl(struct vmw_device *v, struct drm_file *fp,
				  struct drm_vmw_fence_signaled_arg *a)
{
	struct drm_fence *f = drm_fence_handle_lookup(fp, a->handle);

	if (!f)
		return -EINVAL;
	a->signaled = vmw_fence_obj_signaled(v, f) ? 1 : 0;
	/* The flags asked about, as they were asked: every flag of a fence
	 * here passes together. */
	a->signaled_flags = a->flags;
	/* What the driver has seen pass -- on the command-buffer channel the
	 * completions, never the FIFO's register (vmw_fence_check()): user
	 * space marks every fence up to this number done. */
	a->passed_seqno = v->drm.fence_passed;
	drm_fence_put(f);
	return 0;
}

long vmw_fence_obj_unref_ioctl(struct vmw_device *v, struct drm_file *fp,
			       struct drm_vmw_fence_arg *a)
{
	(void)v;
	return drm_fence_handle_delete(fp, a->handle);
}

/* DRM_VMW_FENCE_EVENT asks for a DRM event when a fence signals, so that a
 * client can wait in its own poll() loop on the device rather than blocking
 * in an ioctl.  The event is an action on the fence's record, delivered from
 * the sweep -- which already runs from the device's interrupt and from the
 * poll timer, so the event goes out as soon as the sequence passes with
 * nothing else to arrange. */
long vmw_ioctl_fence_event(struct vmw_device *v, struct drm_file *fp,
			   struct drm_vmw_fence_event_arg *a)
{
	uint64_t user_rep = a->fence_rep;
	struct drm_fence *f;
	uint32_t handle = 0;
	int ret;

	if (a->handle) {
		/* An existing fence; a new handle to it for the reply, which
		 * the caller drops on its own. */
		f = drm_fence_handle_lookup(fp, a->handle);
		if (!f)
			return -EINVAL;
	} else {
		/* A fence for what has been queued. */
		f = vmw_fence_emit(v, 0);
		if (!f)
			return -ENOMEM;
	}
	if (user_rep) {
		ret = drm_fence_handle_create(fp, f, &handle);
		if (ret) {
			drm_fence_put(f);
			return ret;
		}
	}

	ret = vmw_event_fence_action_create(v, fp, f, a->flags, a->user_data);
	if (ret) {
		if (user_rep)
			(void)drm_fence_handle_delete(fp, handle);
		drm_fence_put(f);
		return ret;
	}

	vmw_fence_copy_user_rep(v, fp, 0, user_rep, f, handle, -1);
	drm_fence_put(f);
	return 0;
}

/* ---- set-up ---------------------------------------------------------------------- */

int vmw_fence_manager_init(struct vmw_device *v)
{
	struct vmw_fence_manager *fman = &g_fman;

	mm_memset(fman, 0, sizeof(*fman));
	fman->v = v;
	spinlock_init(&fman->lock, "vmw_fence");
	INIT_LIST_HEAD(&fman->fence_list);
	/* Fences can be emitted from the start: the channel underneath is up
	 * before anything here is called. */
	fman->fifo_down = 0;
	v->fman = fman;
	g_fence_dev = v;
	hrtimer_init(&v->fence_poll, vmw_fence_poll_fire, v);
	return 0;
}

void vmw_fence_fifo_up(struct vmw_device *v)
{
	struct vmw_fence_manager *fman = v->fman;
	uint64_t fl;

	if (!fman)
		return;
	spin_lock_irqsave(&fman->lock, &fl);
	fman->fifo_down = 0;
	spin_unlock_irqrestore(&fman->lock, fl);
}

void vmw_fence_fifo_down(struct vmw_device *v)
{
	struct vmw_fence_manager *fman = v->fman;
	uint64_t fl;

	if (!fman)
		return;
	/* The list changes whenever the lock is dropped, so take the newest
	 * pending fence afresh each time: waiting for it covers everything
	 * before it on the channel.  One the device does not pass in time is
	 * signalled anyway -- nothing will run it now -- and its actions
	 * with it. */
	spin_lock_irqsave(&fman->lock, &fl);
	fman->fifo_down = 1;
	while (!list_empty(&fman->fence_list)) {
		struct vmw_fence_obj *obj =
			list_last_entry(&fman->fence_list, struct vmw_fence_obj,
					head);
		struct drm_fence *f = obj->fence;
		int ret;

		drm_fence_get(f);
		spin_unlock_irqrestore(&fman->lock, fl);

		ret = vmw_fence_obj_wait(v, f, false, false,
					 VMW_FENCE_WAIT_TIMEOUT_NS);
		if (ret)
			drm_fence_signal(f);
		vmw_fence_check(v);
		drm_fence_put(f);

		spin_lock_irqsave(&fman->lock, &fl);
	}
	spin_unlock_irqrestore(&fman->lock, fl);
}

/* Power management.  Down: every pending fence waited for (or signalled
 * past the timeout, see vmw_fence_fifo_down()), the goal forgotten, the
 * interrupt holds given back -- the hardware layer re-programs its mask from
 * the counts on resume, and nothing here needs an interrupt while the device
 * is away -- and the poll timer stopped, `stopping' first, under the lock
 * the timer decides under, so that it cannot re-arm itself behind the
 * cancel. */
void vmw_fence_suspend(struct vmw_device *v)
{
	struct vmw_fence_manager *fman = v->fman;
	uint64_t fl;

	if (!fman)
		return;
	vmw_fence_fifo_down(v);
	spin_lock_irqsave(&fman->lock, &fl);
	fman->stopping = 1;
	fman->seqno_valid = false;
	(void)vmw_fence_irq_reconcile_locked(fman);
	spin_unlock_irqrestore(&fman->lock, fl);
	hrtimer_cancel(&v->fence_poll);
	v->fence_poll_running = 0;
}

/* Up again.  `passed' is the last sequence handed out before the suspend
 * (0: none to record): every fence up to it was waited for or given up on
 * by the suspend, so the device-wide passed value is moved up to it before
 * anything new is emitted -- a device that was reset continues its fence
 * counter from there.  The timer and the interrupt holds come back on
 * demand, with the next fence. */
void vmw_fence_resume(struct vmw_device *v, uint32_t passed)
{
	struct vmw_fence_manager *fman = v->fman;
	uint64_t fl;

	if (!fman)
		return;
	if (passed)
		drm_fence_signal_upto(&v->drm, passed);
	spin_lock_irqsave(&fman->lock, &fl);
	fman->stopping = 0;
	spin_unlock_irqrestore(&fman->lock, fl);
	vmw_fence_fifo_up(v);
	/* Retire what the move above signalled, and run its actions. */
	vmw_fence_check(v);
}

void vmw_fence_manager_takedown(struct vmw_device *v)
{
	struct vmw_fence_manager *fman = v->fman;
	uint64_t fl;

	if (fman) {
		vmw_fence_fifo_down(v);
		spin_lock_irqsave(&fman->lock, &fl);
		fman->stopping = 1;
		fman->seqno_valid = false;
		(void)vmw_fence_irq_reconcile_locked(fman);
		spin_unlock_irqrestore(&fman->lock, fl);
	}
	hrtimer_cancel(&v->fence_poll);
	v->fence_poll_running = 0;
}
