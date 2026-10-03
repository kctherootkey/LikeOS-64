// LikeOS -- display-manager core: the vblank counter and its events.
//
// One counter per crtc, advanced by a timer where the driver cannot count
// (and as a watchdog where it can but stops), or by the driver's own
// vblank interrupt through drm_vblank_tick().  Clients wait on it
// (WAIT_VBLANK), read it (CRTC_GET_SEQUENCE) or ask for an event when it
// reaches a value (WAIT_VBLANK with EVENT, CRTC_QUEUE_SEQUENCE, a page
// flip's completion).  The lifecycle rule below -- one lock, no
// cancellation -- is what keeps the counter from wedging.
//
// A driver may tell the core more about its vblank hardware, and each piece
// is used only when it does: enable_vblank/disable_vblank switch the vblank
// interrupt on for the first drm_crtc_vblank_get and off a while after the
// last put; drm_crtc_vblank_on/off/reset follow the crtc through mode sets
// (off answers what still waits); get_vblank_counter with max_vblank_count
// lets the counter account for vblanks no interrupt reported, with wrap; and
// get_scanout_position dates every vblank from where the scanout is, not
// from when the interrupt was taken (drm_calc_timestamping_constants gives
// the frame and line duration that needs).  Vblank works -- functions run
// on a thread once the counter reaches a value -- are in drm_vblank_work.c.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from the DRM vblank code and Intel's code: MIT
// Portions by Rickard E. (Rik) Faith <faith@valinux.com> and Gareth Hughes <gareth@valinux.com>
// Portions Copyright 2016 Intel Corp.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_vblank.h>
#include <kernel/dev/gpu/drm_vblank_work.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

/* ---- vblank ---------------------------------------------------------- */

static bool vbl_deliver(struct drm_device *dev, int crtc, int from_timer);

/* The vblank lifecycle: one rule, one lock, and no cancellation.
 *
 * Everything that decides whether the timer should run -- `running', `refs',
 * the CRTC's active flag as consulted here -- is read and written under
 * g_vbl_lock.  And nothing ever calls hrtimer_cancel: a timer that is no
 * longer needed is not stopped, it DECLINES TO RE-ARM at its next fire.
 *
 * Both points exist because the previous shape -- unlocked state, synchronous
 * hrtimer_cancel in vbl_stop -- had a race that wedged the counter for good:
 * one processor deciding to stop (running = 0, about to cancel) while another
 * decided to start (sees running == 0, sets it, arms the timer) ends with the
 * first processor CANCELLING THE FRESH TIMER and `running' left at 1.  From
 * then on every start returns immediately ("already running"), the timer is
 * never armed again, the counter never advances, and every waiter loops
 * timeout -> retry for the life of the machine -- seen as a browser thread
 * parked on vbl_wq in every hang dump, and a page load frozen part-way.  The
 * refcounting that made waits safe also made this path hot enough to hit the
 * race within a page load.
 *
 * With termination owned solely by the callback there is nothing to cancel
 * and no ordering to lose: the only writer that arms is whoever moved
 * `running' from 0 to 1 (under the lock), and the only re-armer is the
 * callback itself while `running' stays 1.  The cost is that the counter
 * runs at most one period past its last use.
 *
 * The same holds for the interrupt-off timer of the reference-counted
 * interface (disable_timer): the last put arms it, a later get does not
 * stop it, and when it fires it switches the interrupt off only if there is
 * still no user.  The counter, its timestamp, the hardware counter base and
 * the enabled / inmodeset state are all written under g_vbl_lock too, and
 * the driver's vblank hooks (get_vblank_counter, get_scanout_position,
 * enable_vblank, disable_vblank) are called with it held and interrupts
 * off: they must not sleep or take it. */
static spinlock_t g_vbl_lock = SPINLOCK_INIT("drm_vbl");

/* Pending vblank-event requests: kept on the device, delivered to the
 * requesting file when the counter reaches the target. */
static struct drm_pending_vblank_event *g_vbl_waiters;

/* Somebody waits for this crtc's counter (an event on the list).  Caller
 * holds g_vbl_lock. */
static int vbl_has_waiters_locked(struct drm_device *dev, int crtc)
{
	for (struct drm_pending_vblank_event *w = g_vbl_waiters; w; w = w->next)
		if (w->fp->dev == dev && w->crtc == crtc)
			return 1;
	return 0;
}

/* The references that ask for the counter to count: all of them but the
 * one a crtc that is off holds against being switched back on.  Caller
 * holds g_vbl_lock. */
static int vbl_users_locked(const struct drm_vblank_crtc *v)
{
	return v->refs - (v->inmodeset ? 1 : 0);
}

/* Does the timer have to keep running for this crtc?  Where the core
 * counts, while the crtc runs or a sync waiter needs it.  Where the
 * backend counts (hw_vblank), only while somebody waits: then it is a
 * watchdog that stands in for vblanks the hardware stopped reporting.
 * Caller holds g_vbl_lock. */
static int vbl_timer_needed_locked(struct drm_device *dev, int i)
{
	if (dev->drv->hw_vblank)
		return vbl_users_locked(&dev->vbl[i]) > 0 || vbl_has_waiters_locked(dev, i);
	return dev->crtc[i].active || vbl_users_locked(&dev->vbl[i]) > 0;
}

/* An event request that has been pending for a second or more is said
 * (once per request, a few per boot): a client waiting for a flip or a
 * vblank that never comes is frozen on screen. */
static void vbl_report_late(struct drm_device *dev, int crtc, uint64_t now)
{
	static uint32_t said;
	struct drm_pending_vblank_event *late = NULL;
	struct drm_pending_vblank_event copy;
	uint64_t fl;

	if (said >= 8)
		return;
	spin_lock_irqsave(&g_vbl_lock, &fl);
	for (struct drm_pending_vblank_event *w = g_vbl_waiters; w; w = w->next) {
		if (w->fp->dev == dev && w->crtc == crtc && !w->late_said &&
		    (int64_t)(now - w->due_ns) >= 1000000000LL) {
			w->late_said = 1;
			copy = *w;
			late = &copy;
			break;
		}
	}
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	if (!late)
		return;
	said++;
	kprintf("[drm] %s: crtc %d: a %s event asked for %u ms ago (for vblank %llu) is still not delivered a second after its vblank was due; the counter is at %llu, the crtc is %s, the last hardware vblank was %u ms ago\n",
		dev->drv->name, crtc,
		late->is_flip ? "flip-complete" : late->is_seq ? "crtc-sequence" : "vblank",
		(unsigned)((now - late->queued_ns) / 1000000ULL),
		(unsigned long long)late->target, (unsigned long long)dev->vbl[crtc].count,
		dev->crtc[crtc].active ? "on" : "off",
		dev->vbl[crtc].hw_last_ns ?
			(unsigned)((now - dev->vbl[crtc].hw_last_ns) / 1000000ULL) : 0u);
}

/* ---- counting: hardware counter, timestamps -------------------------- */

static void vbl_disable_timer_fire(hrtimer_t *t);

/* The per-crtc state of the reference-counted interface, filled in the
 * first time anything touches the crtc.  The interrupt-off timer is
 * initialised here, once, before anything can arm it -- like the counter's
 * own timer, it is never initialised again.  Caller holds g_vbl_lock. */
static void vbl_setup_locked(struct drm_device *dev, int pipe)
{
	struct drm_vblank_crtc *v = &dev->vbl[pipe];

	if (v->setup)
		return;
	v->dev = dev;
	v->pipe = pipe;
	v->config.offdelay_ms = DRM_VBLANK_OFFDELAY_MS;
	v->config.disable_immediate = false;
	hrtimer_init(&v->disable_timer, vbl_disable_timer_fire, v);
	v->setup = 1;
}

static uint32_t vbl_max_vblank_count(struct drm_device *dev, int pipe)
{
	struct drm_vblank_crtc *v = &dev->vbl[pipe];

	return v->max_vblank_count ? v->max_vblank_count : dev->drv->max_vblank_count;
}

/* The hardware frame counter, or 0 where there is none (no hook, or no
 * width to wrap it at). */
static uint32_t vbl_hw_counter(struct drm_device *dev, int pipe)
{
	if (dev->drv->get_vblank_counter && vbl_max_vblank_count(dev, pipe))
		return dev->drv->get_vblank_counter(dev, pipe);
	return 0;
}

/* The time of the last vblank: from the scanout position where the driver
 * can say where the scanout is, else now.  True when the time is precise. */
static bool vbl_get_last_vbltimestamp(struct drm_device *dev, int pipe,
				      uint64_t *tvblank, bool in_vblank_irq)
{
	bool ret = false;
	/* the largest error accepted (ns) */
	int max_error = DRM_TIMESTAMP_PRECISION_US * 1000;

	if (dev->drv->get_scanout_position && max_error > 0)
		ret = drm_crtc_vblank_helper_get_vblank_timestamp(dev, pipe, &max_error,
								  tvblank, in_vblank_irq);
	/* No precise time: when we were told is the best there is. */
	if (!ret)
		*tvblank = hrtimer_now_ns();
	return ret;
}

/* The hardware counter and the vblank time as a consistent pair: read
 * again while the counter moved under the timestamp query. */
static bool vbl_get_counter_and_timestamp(struct drm_device *dev, int pipe,
					  uint32_t *cur_vblank, uint64_t *t_vblank,
					  bool in_vblank_irq)
{
	int count = DRM_TIMESTAMP_MAXRETRIES;
	bool rc;

	do {
		*cur_vblank = vbl_hw_counter(dev, pipe);
		rc = vbl_get_last_vbltimestamp(dev, pipe, t_vblank, in_vblank_irq);
	} while (*cur_vblank != vbl_hw_counter(dev, pipe) && --count > 0);
	return rc;
}

/* Advance the counter by `inc', naming the vblank at `t_vblank' (0: not
 * known), with `last' the hardware counter that goes with it.  Caller holds
 * g_vbl_lock. */
static void vbl_store_locked(struct drm_device *dev, int pipe, uint32_t inc,
			     uint64_t t_vblank, uint32_t last)
{
	struct drm_vblank_crtc *v = &dev->vbl[pipe];

	v->last = last;
	if (vbl_max_vblank_count(dev, pipe))
		v->last_valid = 1;
	v->last_ns = t_vblank;
	v->count += inc;
}

/* How many vblanks passed since the counter last moved, and the counter
 * moved by that much.  With a hardware counter the difference of two
 * readings (wrapping at max_vblank_count); with precise timestamps and a
 * known frame duration the time elapsed in frames; otherwise one per
 * vblank interrupt and none outside it.  Where the driver provides none of
 * that, a vblank interrupt (or the timer standing in for one) counts one
 * at the time it was taken -- what the counter always did.
 *
 * Only a crtc whose interrupt goes off between users needs the first two:
 * frames no interrupt reported are then still counted when it comes back
 * on, and a client never sees the counter stand still while the screen
 * refreshed.  Caller holds g_vbl_lock. */
static void drm_update_vblank_count_locked(struct drm_device *dev, int pipe,
					   bool in_vblank_irq)
{
	struct drm_vblank_crtc *v = &dev->vbl[pipe];
	uint32_t cur_vblank, diff;
	bool rc;
	uint64_t t_vblank;
	int framedur_ns = v->framedur_ns;
	uint32_t max_vblank_count = vbl_max_vblank_count(dev, pipe);

	/* Read the counter and the time again until they agree: the
	 * hardware may move its counter while the time is taken.  It is
	 * possible to lose a whole max_vblank_count + 1 vblanks here, with a
	 * narrow counter or an interrupt off for long. */
	rc = vbl_get_counter_and_timestamp(dev, pipe, &cur_vblank, &t_vblank,
					   in_vblank_irq);

	if (max_vblank_count && v->last_valid) {
		/* the hardware counter, where there is one */
		diff = (cur_vblank - v->last) & max_vblank_count;
	} else if (max_vblank_count) {
		/* Its first reading is where counting starts from, not a
		 * difference: against a base of zero it would jump the
		 * counter by whatever the hardware counted since power on. */
		v->last = cur_vblank;
		v->last_valid = 1;
		diff = in_vblank_irq ? 1 : 0;
	} else if (rc && framedur_ns && v->last_ns) {
		/* how many frames fit between this vblank and the last;
		 * a time that did not move forward is the same vblank
		 * reported twice */
		uint64_t diff_ns = t_vblank > v->last_ns ? t_vblank - v->last_ns : 0;

		diff = (uint32_t)DIV_ROUND_CLOSEST_ULL(diff_ns, (uint64_t)framedur_ns);
	} else {
		/* no precise time: one per interrupt */
		diff = in_vblank_irq ? 1 : 0;
	}

	/* Between a pre- and post-modeset notification the hardware counter
	 * may reset and timestamps are not to be trusted: never jump by
	 * more than one there. */
	if (diff > 1 && (v->inmodeset & 0x2))
		diff = 1;

	if (diff == 0) {
		WARN_ON_ONCE(cur_vblank != v->last);
		return;
	}

	/* A time taken outside the interrupt that is not precise does not
	 * name this vblank: 0 marks it unknown until the next interrupt. */
	if (!rc && !in_vblank_irq)
		t_vblank = 0;

	vbl_store_locked(dev, pipe, diff, t_vblank, cur_vblank);
}

/* Back on after a mode set: the timestamp of the current count is the
 * last vblank, and the count moves by one so that no client sees the same
 * value before and after.  Caller holds g_vbl_lock. */
static void vbl_reset_timestamp_locked(struct drm_device *dev, int pipe)
{
	uint32_t cur_vblank;
	uint64_t t_vblank;
	bool rc;

	/* the hardware counter as it stands, so that enabling applies no
	 * stale difference */
	rc = vbl_get_counter_and_timestamp(dev, pipe, &cur_vblank, &t_vblank, false);
	if (!rc)
		t_vblank = 0;
	vbl_store_locked(dev, pipe, 1, t_vblank, cur_vblank);
}

/* Switch the crtc's vblank interrupt off, with the counter brought up to
 * date first so that it reads as if it had counted all along.  Caller holds
 * g_vbl_lock. */
static void vbl_disable_and_save_locked(struct drm_device *dev, int pipe)
{
	struct drm_vblank_crtc *v = &dev->vbl[pipe];

	/* only an interrupt that is on: the driver is not asked to switch
	 * off hardware that may be powered down */
	if (!v->enabled)
		return;
	drm_update_vblank_count_locked(dev, pipe, false);
	if (dev->drv->disable_vblank)
		dev->drv->disable_vblank(dev, pipe);
	v->enabled = 0;
}

/* The interrupt-off timer: nobody took a reference since the last put. */
static void vbl_disable_timer_fire(hrtimer_t *t)
{
	struct drm_vblank_crtc *v = t->arg;
	uint64_t fl;

	if (!v || !v->dev)
		return;
	spin_lock_irqsave(&g_vbl_lock, &fl);
	if (v->refs == 0 && v->enabled)
		vbl_disable_and_save_locked(v->dev, v->pipe);
	spin_unlock_irqrestore(&g_vbl_lock, fl);
}

/* Switch the crtc's vblank interrupt on.  The counter takes in what the
 * hardware counted while it was off.  Without an enable_vblank hook there is
 * nothing to switch: the core's timer or the driver's interrupt counts
 * anyway.  Caller holds g_vbl_lock. */
static int vbl_enable_locked(struct drm_device *dev, int pipe)
{
	struct drm_vblank_crtc *v = &dev->vbl[pipe];
	int ret = 0;

	if (v->enabled)
		return 0;
	if (dev->drv->enable_vblank)
		ret = dev->drv->enable_vblank(dev, pipe);
	if (ret)
		return ret;
	drm_update_vblank_count_locked(dev, pipe, false);
	v->enabled = 1;
	/* The interrupt was off on purpose; the watchdog counts from now,
	 * not from the last vblank before it went off. */
	if (dev->drv->hw_vblank && dev->drv->enable_vblank)
		v->hw_last_ns = hrtimer_now_ns();
	return 0;
}

void drm_vbl_timer_fire(hrtimer_t *t)
{
	struct drm_device *dev = t->arg;
	/* which crtc's timer: in bytes, since the timers are a whole
	 * element apart (an hrtimer_t difference divided by the element
	 * size named crtc 0 for every crtc) */
	int i = (int)(((char *)t - (char *)&dev->vbl[0].timer) /
		      ((char *)&dev->vbl[1] - (char *)&dev->vbl[0]));

	if (i < 0 || (uint32_t)i >= dev->ncrtc)
		return;
	uint64_t now = hrtimer_now_ns();
	uint64_t period = dev->vbl[i].period_ns ? dev->vbl[i].period_ns : 16666666ULL;
	if (!dev->drv->hw_vblank) {
		vbl_deliver(dev, i, 1);
	} else if (dev->crtc[i].active && now - dev->vbl[i].hw_last_ns >= 3 * period) {
		/* The backend counts, and has not for three periods while
		 * somebody waits: a vblank interrupt that does not arrive
		 * (a masked pipe interrupt, a power well that came back
		 * without its enables) must not freeze every client that
		 * presents.  The timer counts until the hardware does again. */
		if (!dev->vbl[i].soft) {
			dev->vbl[i].soft = 1;
			if (dev->drv->vblank_report)
				dev->drv->vblank_report(dev, i);
			if (dev->vbl[i].soft_said < 4) {
				dev->vbl[i].soft_said++;
				kprintf("[drm] %s: crtc %d is on but no vblank interrupt came for %u ms while a client waits; vblanks are counted from a timer until one arrives\n",
					dev->drv->name, i,
					dev->vbl[i].hw_last_ns ?
						(unsigned)((now - dev->vbl[i].hw_last_ns) / 1000000ULL) :
						0u);
			}
		}
		vbl_deliver(dev, i, 1);
	}
	if (dev->drv->hw_vblank)
		vbl_report_late(dev, i, now);
	{
		uint64_t fl;
		int keep;

		spin_lock_irqsave(&g_vbl_lock, &fl);
		keep = vbl_timer_needed_locked(dev, i);
		if (!keep)
			dev->vbl[i].running = 0;
		spin_unlock_irqrestore(&g_vbl_lock, fl);
		if (keep)
			hrtimer_start(&dev->vbl[i].timer,
				      dev->drv->hw_vblank ? now + period :
							    dev->vbl[i].last_ns + period);
	}
}

static void vbl_flush_inactive(struct drm_device *dev, int crtc);

/* Make the counter run if anything needs it.  Idempotent, callable from any
 * process context; the timer itself was initialised once at drm_kms_init and
 * is never re-initialised (re-initialising a queued hrtimer corrupts the
 * timer list, which is why vbl_start doing it on every start had to go). */
void drm_kms_vbl_sync(struct drm_device *dev, int i)
{
	uint64_t fl;
	int arm = 0;

	/* a crtc that goes off takes no more vblanks: what waits for one
	 * is answered now, with the count as it stands */
	if (!dev->crtc[i].active)
		vbl_flush_inactive(dev, i);
	spin_lock_irqsave(&g_vbl_lock, &fl);
	if (!dev->vbl[i].running && vbl_timer_needed_locked(dev, i)) {
		dev->vbl[i].running = 1;
		if (!dev->drv->hw_vblank)
			dev->vbl[i].last_ns = hrtimer_now_ns();
		else if (!dev->vbl[i].hw_last_ns)
			dev->vbl[i].hw_last_ns = hrtimer_now_ns();
		arm = 1;
	}
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	if (arm) {
		uint64_t period = dev->vbl[i].period_ns ? dev->vbl[i].period_ns : 16666666ULL;
		hrtimer_start(&dev->vbl[i].timer,
			      dev->drv->hw_vblank ? hrtimer_now_ns() + period :
						    dev->vbl[i].last_ns + period);
	}
}

/* ---- references ------------------------------------------------------ */

int drm_crtc_vblank_get(struct drm_device *dev, int pipe)
{
	struct drm_vblank_crtc *v;
	uint64_t fl;
	int ret = 0;

	if (!dev->ncrtc)
		return -EINVAL;
	if (WARN_ON(pipe < 0 || (uint32_t)pipe >= dev->ncrtc))
		return -EINVAL;
	v = &dev->vbl[pipe];
	spin_lock_irqsave(&g_vbl_lock, &fl);
	vbl_setup_locked(dev, pipe);
	/* the first user switches the interrupt on; a failure gives the
	 * reference back */
	if (++v->refs == 1) {
		ret = vbl_enable_locked(dev, pipe);
		if (ret)
			v->refs--;
	} else if (!v->enabled && (v->managed || dev->drv->enable_vblank)) {
		/* off (drm_crtc_vblank_off), or the interrupt could not be
		 * switched on: no vblank is coming */
		v->refs--;
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	if (!ret)
		drm_kms_vbl_sync(dev, pipe);
	return ret;
}

void drm_crtc_vblank_put(struct drm_device *dev, int pipe)
{
	struct drm_vblank_crtc *v;
	uint64_t fl;
	int arm = 0;
	int delay;

	if (WARN_ON(pipe < 0 || (uint32_t)pipe >= dev->ncrtc))
		return;
	v = &dev->vbl[pipe];
	spin_lock_irqsave(&g_vbl_lock, &fl);
	vbl_setup_locked(dev, pipe);
	if (WARN_ON_ONCE(v->refs <= 0)) {
		spin_unlock_irqrestore(&g_vbl_lock, fl);
		return;
	}
	delay = v->config.offdelay_ms;
	/* The last user lets the interrupt go off -- only where the driver
	 * can switch it.  Otherwise there is nothing to stop: if this was the
	 * last need, the counter's timer notices at its next fire and lets
	 * itself die there. */
	if (--v->refs == 0 && dev->drv->disable_vblank && v->enabled) {
		if (delay < 0)
			vbl_disable_and_save_locked(dev, pipe);
		else if (delay > 0 && !v->config.disable_immediate)
			arm = 1;
	}
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	if (arm)
		hrtimer_start(&v->disable_timer,
			      hrtimer_now_ns() + (uint64_t)delay * 1000000ULL);
}

/* ---- events ------------------------------------------------------------ */

/* One event out to its file, with the count and time it is answered at;
 * the request is freed. */
static void vbl_send(struct drm_device *dev, struct drm_pending_vblank_event *w,
		     uint64_t count, uint64_t now)
{
	if (w->is_seq) {
		struct drm_event_crtc_sequence ev;
		mm_memset(&ev, 0, sizeof(ev));
		ev.base.type = DRM_EVENT_CRTC_SEQUENCE;
		ev.base.length = sizeof(ev);
		ev.user_data = w->user_data;
		ev.time_ns = (int64_t)now;
		ev.sequence = count;
		drm_event_queue(w->fp, &ev, sizeof(ev));
	} else {
		struct drm_event_vblank ev;
		mm_memset(&ev, 0, sizeof(ev));
		ev.base.type = w->is_flip ? DRM_EVENT_FLIP_COMPLETE : DRM_EVENT_VBLANK;
		ev.base.length = sizeof(ev);
		ev.user_data = w->user_data;
		ev.tv_sec = (uint32_t)(now / 1000000000ULL);
		ev.tv_usec = (uint32_t)((now % 1000000000ULL) / 1000);
		ev.sequence = (uint32_t)count;
		ev.crtc_id = dev->crtc[w->crtc].id;
		drm_event_queue(w->fp, &ev, sizeof(ev));
	}
	kfree(w);
}

/* Send a list of events taken off g_vbl_waiters (linked through next),
 * each dropping the reference it carried.  Without the lock. */
static void vbl_send_list(struct drm_device *dev, int crtc,
			  struct drm_pending_vblank_event *due, uint64_t count,
			  uint64_t at)
{
	while (due) {
		struct drm_pending_vblank_event *w = due;
		due = w->next;
		if (w->holds_ref)
			drm_crtc_vblank_put(dev, crtc);
		vbl_send(dev, w, count, at);
	}
}

/* A vblank: count it and answer what waited for it.  `from_timer': the
 * core's timer is the vblank (it counts one even where the hardware
 * counter did not move -- a stalled pipe must not freeze its clients).
 * False when the crtc's vblank handling is off. */
static bool vbl_deliver(struct drm_device *dev, int crtc, int from_timer)
{
	struct drm_vblank_crtc *v = &dev->vbl[crtc];
	uint64_t fl;
	bool disable_irq;

	spin_lock_irqsave(&g_vbl_lock, &fl);
	vbl_setup_locked(dev, crtc);
	/* Switched off by the driver (drm_crtc_vblank_off, or the interrupt
	 * went off after its last user): nothing to count. */
	if (v->managed && !v->enabled) {
		spin_unlock_irqrestore(&g_vbl_lock, fl);
		return false;
	}
	uint64_t before = v->count;
	drm_update_vblank_count_locked(dev, crtc, true);
	if (from_timer && v->count == before) {
		v->count++;
		v->last_ns = hrtimer_now_ns();
	}
	uint64_t count = v->count;
	uint64_t now = v->last_ns;
	struct drm_pending_vblank_event **pp = &g_vbl_waiters, *due = NULL;
	while (*pp) {
		struct drm_pending_vblank_event *w = *pp;
		if (w->fp->dev == dev && w->crtc == crtc &&
		    (int64_t)(count - w->target) >= 0) {
			*pp = w->next;
			w->next = due;
			due = w;
			continue;
		}
		pp = &w->next;
	}
	/* Interrupt off at the vblank after the last user (where the
	 * counter stays exact across that): after the events, so that
	 * their timestamps are this vblank's. */
	disable_irq = v->config.disable_immediate && v->config.offdelay_ms > 0 &&
		      v->refs == 0 && v->enabled && dev->drv->disable_vblank;
	spin_unlock_irqrestore(&g_vbl_lock, fl);

	vbl_send_list(dev, crtc, due, count, now);
	poll_notify_wq(&dev->vbl_wq);
	drm_handle_vblank_works(dev, crtc);
	if (disable_irq) {
		spin_lock_irqsave(&g_vbl_lock, &fl);
		if (v->refs == 0 && v->enabled)
			vbl_disable_and_save_locked(dev, crtc);
		spin_unlock_irqrestore(&g_vbl_lock, fl);
	}
	return true;
}

/* Every request of a crtc that is off: answered now. */
static void vbl_flush_inactive(struct drm_device *dev, int crtc)
{
	uint64_t fl;
	struct drm_pending_vblank_event **pp, *due = NULL;

	spin_lock_irqsave(&g_vbl_lock, &fl);
	pp = &g_vbl_waiters;
	while (*pp) {
		struct drm_pending_vblank_event *w = *pp;
		if (w->fp->dev == dev && w->crtc == crtc) {
			*pp = w->next;
			w->next = due;
			due = w;
			continue;
		}
		pp = &w->next;
	}
	uint64_t count = dev->vbl[crtc].count;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	uint64_t now = hrtimer_now_ns();
	vbl_send_list(dev, crtc, due, count, now);
	poll_notify_wq(&dev->vbl_wq);
}

bool drm_crtc_handle_vblank(struct drm_device *dev, int crtc)
{
	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return false;
	dev->vbl[crtc].hw_last_ns = hrtimer_now_ns();
	if (dev->vbl[crtc].soft) {
		/* the hardware counts again; the timer stands down (said
		 * from the next timer pass, not from this interrupt) */
		dev->vbl[crtc].soft = 0;
	}
	return vbl_deliver(dev, crtc, 0);
}

void drm_vblank_tick(struct drm_device *dev, int crtc)
{
	(void)drm_crtc_handle_vblank(dev, crtc);
}

/* Put an event on the list for its target, or -- when the target has
 * already passed, or the crtc counts no more -- answer it at once.  1 and
 * the count it was answered with when it went out at once.  A reference the
 * event holds goes with it.  `next': the target is the vblank after the
 * current one, set here under the lock -- with the count first brought up
 * to date from the hardware where the event holds a reference (the
 * interrupt is on then, and the hardware can be asked). */
static int vbl_queue_at(struct drm_device *dev, struct drm_pending_vblank_event *w,
			uint64_t *answered, int next)
{
	struct drm_vblank_crtc *v = &dev->vbl[w->crtc];
	int crtc = w->crtc;
	uint64_t fl;

	w->queued_ns = hrtimer_now_ns();
	w->late_said = 0;
	spin_lock_irqsave(&g_vbl_lock, &fl);
	if (next) {
		if (w->holds_ref)
			drm_update_vblank_count_locked(dev, crtc, false);
		w->target = v->count + 1;
	}
	uint64_t count = v->count;
	{
		uint64_t period = v->period_ns ? v->period_ns : 16666666ULL;
		uint64_t ahead = (int64_t)(w->target - count) > 0 ? w->target - count : 0;
		if (ahead > 1000000)
			ahead = 1000000;
		w->due_ns = w->queued_ns + ahead * period;
	}
	/* A vblank already passed (an absolute target behind the counter),
	 * or a crtc that is off and will count no more: answered at once,
	 * as the interface promises -- queued, it would wait for a vblank
	 * that is never going to match. */
	if ((int64_t)(count - w->target) >= 0 || !dev->crtc[crtc].active ||
	    (v->managed && !v->enabled)) {
		uint64_t at = v->last_ns ? v->last_ns : w->queued_ns;
		spin_unlock_irqrestore(&g_vbl_lock, fl);
		if (answered)
			*answered = count;
		if (w->holds_ref)
			drm_crtc_vblank_put(dev, crtc);
		vbl_send(dev, w, count, at);
		return 1;
	}
	w->next = g_vbl_waiters;
	g_vbl_waiters = w;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	drm_kms_vbl_sync(dev, crtc);
	return 0;
}

static int vbl_queue(struct drm_device *dev, struct drm_pending_vblank_event *w,
		     uint64_t *answered)
{
	return vbl_queue_at(dev, w, answered, 0);
}

struct drm_pending_vblank_event *drm_vblank_event_create(struct drm_file *fp,
							 uint32_t type,
							 uint64_t user_data)
{
	struct drm_pending_vblank_event *e = kalloc(sizeof(*e));

	if (!e)
		return NULL;
	mm_memset(e, 0, sizeof(*e));
	e->fp = fp;
	e->user_data = user_data;
	e->is_seq = type == DRM_EVENT_CRTC_SEQUENCE;
	e->is_flip = type == DRM_EVENT_FLIP_COMPLETE;
	return e;
}

int drm_vbl_queue_event(struct drm_device *dev, struct drm_file *fp, int crtc,
			uint64_t target, uint64_t user_data, int is_seq,
			int is_flip)
{
	struct drm_pending_vblank_event *w =
		drm_vblank_event_create(fp, is_seq ? DRM_EVENT_CRTC_SEQUENCE :
					is_flip ? DRM_EVENT_FLIP_COMPLETE :
						  DRM_EVENT_VBLANK,
					user_data);

	if (!w)
		return -ENOMEM;
	w->crtc = crtc;
	w->target = target;
	/* Where the driver switches the vblank interrupt, the event keeps
	 * it on until it is sent.  A crtc that refuses is off, and the
	 * event is answered at once below. */
	if (dev->drv->enable_vblank && drm_crtc_vblank_get(dev, crtc) == 0)
		w->holds_ref = 1;
	(void)vbl_queue(dev, w, NULL);
	return 0;
}

/* A flip completes at the vblank after the one the hardware is in when it
 * is queued.  Where the driver switches its vblank interrupt off between
 * users the count may be stale until the event's reference switches it on
 * again (and the count takes in what the hardware counted meanwhile), so
 * the reference comes first and the target is taken from the count after
 * it: aimed from the stale count, the event would be answered at once,
 * before the flip was shown.  Where the driver has no such switch the
 * counter always counts and the target is simply the next count. */
int drm_kms_queue_flip_event(struct drm_device *dev, struct drm_file *fp,
			     int crtc, uint64_t user_data)
{
#if DRM_FLIP_EVENT_VBLANK_REF
	struct drm_pending_vblank_event *w;

	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return -EINVAL;
	w = drm_vblank_event_create(fp, DRM_EVENT_FLIP_COMPLETE, user_data);
	if (!w)
		return -ENOMEM;
	w->crtc = crtc;
	/* A crtc that refuses the reference is off: the event is answered
	 * at once below. */
	if (dev->drv->enable_vblank && drm_crtc_vblank_get(dev, crtc) == 0)
		w->holds_ref = 1;
	(void)vbl_queue_at(dev, w, NULL, 1);
	return 0;
#else
	return drm_vbl_queue_event(dev, fp, crtc, dev->vbl[crtc].count + 1, user_data,
			       0, 1);
#endif
}

void drm_crtc_send_vblank_event(struct drm_device *dev, int crtc,
				struct drm_pending_vblank_event *e)
{
	uint64_t seq, now;

	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc)) {
		kfree(e);
		return;
	}
	seq = drm_crtc_vblank_count_and_time(dev, crtc, &now);
	if (!now)
		now = hrtimer_now_ns();
	e->crtc = crtc;
	vbl_send(dev, e, seq, now);
}

void drm_crtc_arm_vblank_event(struct drm_device *dev, int crtc,
			       struct drm_pending_vblank_event *e)
{
	struct drm_vblank_crtc *v;
	uint64_t fl;

	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc)) {
		kfree(e);
		return;
	}
	v = &dev->vbl[crtc];
	e->crtc = crtc;
	e->holds_ref = 1; /* the caller's drm_crtc_vblank_get */
	e->queued_ns = hrtimer_now_ns();
	e->late_said = 0;
	spin_lock_irqsave(&g_vbl_lock, &fl);
	/* the vblank after the one the hardware is in now */
	drm_update_vblank_count_locked(dev, crtc, false);
	e->target = v->count + 1;
	e->due_ns = e->queued_ns + (v->period_ns ? v->period_ns : 16666666ULL);
	e->next = g_vbl_waiters;
	g_vbl_waiters = e;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
}

/* Events a file was waiting for go when it does (drm_kms_file_release). */
void drm_vblank_file_release(struct drm_device *dev, struct drm_file *fp)
{
	uint64_t fl;
	struct drm_pending_vblank_event *gone = NULL;

	spin_lock_irqsave(&g_vbl_lock, &fl);
	struct drm_pending_vblank_event **pp = &g_vbl_waiters;
	while (*pp) {
		struct drm_pending_vblank_event *w = *pp;
		if (w->fp == fp) {
			*pp = w->next;
			w->next = gone;
			gone = w;
			continue;
		}
		pp = &w->next;
	}
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	while (gone) {
		struct drm_pending_vblank_event *w = gone;
		gone = w->next;
		if (w->holds_ref)
			drm_crtc_vblank_put(dev, w->crtc);
		kfree(w);
	}
}

/* ---- waiting ----------------------------------------------------------- */

/* Still waiting for `target'?  Not on a crtc the driver switched off. */
static int vbl_still_waiting(struct drm_device *dev, int crtc, uint64_t target)
{
	struct drm_vblank_crtc *v = &dev->vbl[crtc];

	if (v->managed && !v->enabled)
		return 0;
	return (int64_t)(v->count - target) < 0;
}

/* Block until the counter passes `target' (WAIT_VBLANK without EVENT), for
 * at most `ms'.  The caller holds a vblank reference. */
static int vbl_wait_until(struct drm_device *dev, int crtc, uint64_t target,
			  unsigned ms, int interruptible)
{
	task_t *cur = sched_current();
	/* Capped rather than trusting the counter to arrive: a target that
	 * can never be reached must not park a thread for the life of the
	 * process. */
	uint64_t deadline = timer_ticks() + timer_ms_to_ticks(ms) + 1;
	int rc = 0;

	while (vbl_still_waiting(dev, crtc, target)) {
		if (interruptible && cur && signal_pending(cur)) {
			rc = -EINTR;
			break;
		}
		if ((int64_t)(timer_ticks() - deadline) >= 0) {
			rc = -EBUSY;
			break;
		}
		if (!cur) {
			__asm__ volatile("pause");
			continue;
		}
		struct wait_queue_entry we;
		uint64_t fl = local_irq_save();
		wq_entry_init(&we, cur);
		wq_add(&dev->vbl_wq, &we);
		if (vbl_still_waiting(dev, crtc, target)) {
			cur->wait_channel = &dev->vbl_wq;
			cur->wakeup_tick = timer_ticks() + timer_ms_to_ticks(50) + 1;
			cur->state = TASK_BLOCKED;
			local_irq_restore(fl);
			sched_schedule();
			/* Disarm.  A deadline outlives the wait that set it:
			 * sched_wake_expired_sleepers() matches any BLOCKED
			 * task whose wakeup_tick is non-zero and in the past,
			 * with no channel to qualify it, so one left behind
			 * here is claimed on the next tick of whatever this
			 * task parks on NEXT -- a pipe read, an rwsem, a futex,
			 * none of which arm a deadline of their own and so none
			 * of which overwrite it.  The task then spins at the
			 * tick rate on a wait that should have been quiet.
			 * Whoever woke us has usually cleared it already; doing
			 * it here covers the timeout case, which has not. */
			cur->wakeup_tick = 0;
			cur->wait_channel = NULL;
		} else {
			local_irq_restore(fl);
		}
		wq_remove(&dev->vbl_wq, &we);
	}
	return rc;
}

int drm_crtc_wait_one_vblank(struct drm_device *dev, int crtc)
{
	uint64_t last;
	int ret;

	ret = drm_crtc_vblank_get(dev, crtc);
	if (ret) {
		WARN_RATELIMIT(1, "[drm] %s: vblank not available on crtc %d, ret=%d\n",
			       dev->drv->name, crtc, ret);
		return ret;
	}
	last = drm_crtc_vblank_count(dev, crtc);
	ret = vbl_wait_until(dev, crtc, last + 1, 1000, 0);
	if (ret == 0 && drm_crtc_vblank_count(dev, crtc) == last)
		ret = -EINVAL; /* switched off while waiting */
	WARN_RATELIMIT(ret == -EBUSY, "[drm] %s: vblank wait timed out on crtc %d\n",
		       dev->drv->name, crtc);
	drm_crtc_vblank_put(dev, crtc);
	return ret == -EBUSY ? -ETIMEDOUT : ret;
}

/* ---- the counter as clients and drivers read it ------------------------ */

uint64_t drm_crtc_vblank_count(struct drm_device *dev, int crtc)
{
	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc))
		return 0;
	return __atomic_load_n(&dev->vbl[crtc].count, __ATOMIC_ACQUIRE);
}

uint64_t drm_crtc_vblank_count_and_time(struct drm_device *dev, int crtc,
					uint64_t *vblanktime_ns)
{
	uint64_t fl, count;

	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc)) {
		*vblanktime_ns = 0;
		return 0;
	}
	spin_lock_irqsave(&g_vbl_lock, &fl);
	count = dev->vbl[crtc].count;
	*vblanktime_ns = dev->vbl[crtc].last_ns;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	return count;
}

uint64_t drm_crtc_accurate_vblank_count(struct drm_device *dev, int crtc)
{
	uint64_t fl, count;

	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc))
		return 0;
	spin_lock_irqsave(&g_vbl_lock, &fl);
	vbl_setup_locked(dev, crtc);
	drm_update_vblank_count_locked(dev, crtc, false);
	count = dev->vbl[crtc].count;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	return count;
}

struct wait_queue_head *drm_crtc_vblank_waitqueue(struct drm_device *dev,
						  int crtc)
{
	(void)crtc;
	return &dev->vbl_wq;
}

int drm_crtc_next_vblank_start(struct drm_device *dev, int crtc,
			       uint64_t *vblanktime_ns)
{
	struct drm_vblank_crtc *v;
	uint64_t vblank_start;

	if (!dev->ncrtc || crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return -EINVAL;
	v = &dev->vbl[crtc];
	if (!v->framedur_ns || !v->linedur_ns || !v->hwmode.crtc_vtotal)
		return -EINVAL;
	if (!vbl_get_last_vbltimestamp(dev, crtc, vblanktime_ns, false))
		return -EINVAL;
	vblank_start = div64_u64((uint64_t)v->framedur_ns * v->hwmode.crtc_vblank_start,
				 v->hwmode.crtc_vtotal);
	*vblanktime_ns += vblank_start;
	return 0;
}

/* ---- on / off ------------------------------------------------------------ */

void drm_crtc_vblank_off(struct drm_device *dev, int crtc)
{
	struct drm_vblank_crtc *v;
	struct drm_pending_vblank_event **pp, *due = NULL;
	uint64_t fl, seq, now;

	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc))
		return;
	v = &dev->vbl[crtc];
	spin_lock_irqsave(&g_vbl_lock, &fl);
	vbl_setup_locked(dev, crtc);
	v->managed = 1;
	vbl_disable_and_save_locked(dev, crtc);
	/* No drm_crtc_vblank_get may switch the interrupt back on: one
	 * reference is held until drm_crtc_vblank_on. */
	if (!v->inmodeset) {
		v->refs++;
		v->inmodeset = 1;
	}
	/* What still waits is answered now, with the last count, rather
	 * than left for a vblank that does not come. */
	seq = v->count;
	now = v->last_ns ? v->last_ns : hrtimer_now_ns();
	pp = &g_vbl_waiters;
	while (*pp) {
		struct drm_pending_vblank_event *w = *pp;
		if (w->fp->dev == dev && w->crtc == crtc) {
			*pp = w->next;
			w->next = due;
			due = w;
			continue;
		}
		pp = &w->next;
	}
	/* set again with the timing of the next mode
	 * (drm_calc_timestamping_constants) */
	v->hwmode.crtc_clock = 0;
	spin_unlock_irqrestore(&g_vbl_lock, fl);

	/* sync waiters see the crtc off and return */
	poll_notify_wq(&dev->vbl_wq);
	vbl_send_list(dev, crtc, due, seq, now);
	/* works waiting for a vblank go, works already running finish */
	drm_vblank_cancel_pending_works(dev, crtc);
	drm_vblank_flush_worker(dev, crtc);
}

void drm_crtc_vblank_reset(struct drm_device *dev, int crtc)
{
	struct drm_vblank_crtc *v;
	uint64_t fl;
	int waiting;

	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc))
		return;
	v = &dev->vbl[crtc];
	spin_lock_irqsave(&g_vbl_lock, &fl);
	vbl_setup_locked(dev, crtc);
	v->managed = 1;
	/* as off, for a crtc whose interrupt was never on */
	if (!v->inmodeset) {
		v->refs++;
		v->inmodeset = 1;
	}
	waiting = vbl_has_waiters_locked(dev, crtc);
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	WARN_ON_ONCE(waiting);
}

void drm_crtc_vblank_on_config(struct drm_device *dev, int crtc,
			       const struct drm_vblank_crtc_config *config)
{
	struct drm_vblank_crtc *v;
	uint64_t fl;

	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc))
		return;
	v = &dev->vbl[crtc];
	spin_lock_irqsave(&g_vbl_lock, &fl);
	vbl_setup_locked(dev, crtc);
	v->managed = 1;
	v->config = *config;
	/* drop the reference that kept it off */
	if (v->inmodeset) {
		v->refs--;
		v->inmodeset = 0;
	}
	vbl_reset_timestamp_locked(dev, crtc);
	/* The interrupt on again where somebody still holds a reference,
	 * where it is never to go off, or where the driver has no switch
	 * for it (the core counts regardless then). */
	if (v->refs != 0 || !v->config.offdelay_ms || !dev->drv->enable_vblank)
		WARN_ON(vbl_enable_locked(dev, crtc));
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	drm_kms_vbl_sync(dev, crtc);
}

void drm_crtc_vblank_on(struct drm_device *dev, int crtc)
{
	const struct drm_vblank_crtc_config config = {
		.offdelay_ms = DRM_VBLANK_OFFDELAY_MS,
		.disable_immediate = false,
	};

	drm_crtc_vblank_on_config(dev, crtc, &config);
}

void drm_crtc_set_max_vblank_count(struct drm_device *dev, int crtc,
				   uint32_t max_vblank_count)
{
	struct drm_vblank_crtc *v;
	uint64_t fl;

	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc))
		return;
	v = &dev->vbl[crtc];
	WARN_ON_ONCE(dev->drv->max_vblank_count);
	spin_lock_irqsave(&g_vbl_lock, &fl);
	vbl_setup_locked(dev, crtc);
	WARN_ON_ONCE(!v->inmodeset);
	v->max_vblank_count = max_vblank_count;
	v->last_valid = 0;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
}

void drm_crtc_vblank_restore(struct drm_device *dev, int crtc)
{
	struct drm_vblank_crtc *v;
	uint64_t fl, t_vblank, diff_ns;
	uint32_t cur_vblank, diff = 1, max_vblank_count;
	int framedur_ns;

	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc))
		return;
	v = &dev->vbl[crtc];
	WARN_ON_ONCE(!dev->drv->get_scanout_position);
	WARN_ON_ONCE(v->inmodeset);
	WARN_ON_ONCE(!v->config.disable_immediate);
	spin_lock_irqsave(&g_vbl_lock, &fl);
	vbl_setup_locked(dev, crtc);
	max_vblank_count = vbl_max_vblank_count(dev, crtc);
	framedur_ns = v->framedur_ns;
	vbl_get_counter_and_timestamp(dev, crtc, &cur_vblank, &t_vblank, false);
	/* the frames that passed since the last vblank the counter took in,
	 * from the time: the hardware counter is rebased so that the next
	 * reading adds exactly those */
	diff_ns = t_vblank > v->last_ns ? t_vblank - v->last_ns : 0;
	if (framedur_ns)
		diff = (uint32_t)DIV_ROUND_CLOSEST_ULL(diff_ns, (uint64_t)framedur_ns);
	v->last = (cur_vblank - diff) & max_vblank_count;
	if (max_vblank_count)
		v->last_valid = 1;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
}

/* ---- timing constants and precise timestamps ----------------------------- */

void drm_calc_timestamping_constants(struct drm_device *dev, int crtc,
				     const struct drm_display_mode *mode)
{
	struct drm_vblank_crtc *v;
	int linedur_ns = 0, framedur_ns = 0;
	int dotclock = mode->crtc_clock;
	uint64_t fl;

	if (!dev->ncrtc)
		return;
	if (WARN_ON(crtc < 0 || (uint32_t)crtc >= dev->ncrtc))
		return;
	v = &dev->vbl[crtc];

	if (dotclock > 0) {
		uint64_t frame_size = (uint64_t)mode->crtc_htotal * mode->crtc_vtotal;

		/* Line and frame duration in ns from the line length in
		 * pixels and the pixel clock in kHz. */
		linedur_ns = (int)div_u64((uint64_t)mode->crtc_htotal * 1000000, (uint32_t)dotclock);
		framedur_ns = (int)div_u64(frame_size * 1000000, (uint32_t)dotclock);
		/* An interlaced mode scans a field, half a frame. */
		if (mode->flags & DRM_MODE_FLAG_INTERLACE)
			framedur_ns /= 2;
	} else {
		static int said;

		if (said < 4) {
			said++;
			kprintf("[drm] %s: crtc %d: no timing constants without a pixel clock\n",
				dev->drv->name, crtc);
		}
	}

	spin_lock_irqsave(&g_vbl_lock, &fl);
	v->linedur_ns = linedur_ns;
	v->framedur_ns = framedur_ns;
	v->hwmode = *mode;
	/* the timer's period (where it counts or stands in) follows the
	 * frame the crtc really scans */
	if (framedur_ns > 0)
		v->period_ns = (uint64_t)framedur_ns;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
}

/* The hardware timing of a client mode, as programmed without adjustment:
 * the blanking spelled out, double scan and multi scan repeating lines. */
static void vbl_mode_crtc_timing(struct drm_display_mode *p)
{
	p->crtc_clock = p->clock;
	p->crtc_hdisplay = p->hdisplay;
	p->crtc_hsync_start = p->hsync_start;
	p->crtc_hsync_end = p->hsync_end;
	p->crtc_htotal = p->htotal;
	p->crtc_hskew = p->hskew;
	p->crtc_vdisplay = p->vdisplay;
	p->crtc_vsync_start = p->vsync_start;
	p->crtc_vsync_end = p->vsync_end;
	p->crtc_vtotal = p->vtotal;
	if (p->flags & DRM_MODE_FLAG_DBLSCAN) {
		p->crtc_vdisplay *= 2;
		p->crtc_vsync_start *= 2;
		p->crtc_vsync_end *= 2;
		p->crtc_vtotal *= 2;
	}
	if (p->vscan > 1) {
		p->crtc_vdisplay *= p->vscan;
		p->crtc_vsync_start *= p->vscan;
		p->crtc_vsync_end *= p->vscan;
		p->crtc_vtotal *= p->vscan;
	}
	p->crtc_vblank_start = min(p->crtc_vsync_start, p->crtc_vdisplay);
	p->crtc_vblank_end = max(p->crtc_vsync_end, p->crtc_vtotal);
	p->crtc_hblank_start = min(p->crtc_hsync_start, p->crtc_hdisplay);
	p->crtc_hblank_end = max(p->crtc_hsync_end, p->crtc_htotal);
}

void drm_calc_timestamping_constants_umode(struct drm_device *dev, int crtc,
					   const struct drm_mode_modeinfo *umode)
{
	struct drm_display_mode *m = kalloc(sizeof(*m));

	if (!m)
		return;
	if (drm_mode_from_umode(m, umode) == 0) {
		vbl_mode_crtc_timing(m);
		drm_calc_timestamping_constants(dev, crtc, m);
	}
	kfree(m);
}

bool drm_crtc_vblank_helper_get_vblank_timestamp_internal(
	struct drm_device *dev, int crtc, int *max_error,
	uint64_t *vblank_time_ns, bool in_vblank_irq,
	drm_vblank_get_scanout_position_func get_scanout_position)
{
	const struct drm_display_mode *mode;
	uint64_t stime = 0, etime = 0;
	int vpos = 0, hpos = 0, i;
	int64_t delta_ns, duration_ns = 0;

	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return false;
	/* no scanout position query: nothing precise to say */
	if (!get_scanout_position)
		return false;
	mode = &dev->vbl[crtc].hwmode;
	/* timing not known yet (the crtc's first mode set) */
	if (mode->crtc_clock == 0)
		return false;

	/* The scanout position with the system time around the query;
	 * asked again when one query took longer than the error accepted,
	 * which bounds the error if the query was delayed or interrupted. */
	for (i = 0; i < DRM_TIMESTAMP_MAXRETRIES; i++) {
		if (!get_scanout_position(dev, crtc, in_vblank_irq, &vpos, &hpos,
					  &stime, &etime, mode))
			return false;
		/* the uncertainty of this reading */
		duration_ns = (int64_t)(etime - stime);
		if (duration_ns <= *max_error)
			break;
	}
	/* the error bound actually reached */
	*max_error = (int)duration_ns;

	/* Time since the start of scanout at the first line when the
	 * position was read; negative while still in the vblank before it. */
	delta_ns = div_s64(1000000LL * ((int64_t)vpos * mode->crtc_htotal + hpos),
			   mode->crtc_clock);
	/* the end of the vblank: the reading's time less that */
	*vblank_time_ns = etime - (uint64_t)delta_ns;
	return true;
}

bool drm_crtc_vblank_helper_get_vblank_timestamp(struct drm_device *dev,
						 int crtc, int *max_error,
						 uint64_t *vblank_time_ns,
						 bool in_vblank_irq)
{
	return drm_crtc_vblank_helper_get_vblank_timestamp_internal(
		dev, crtc, max_error, vblank_time_ns, in_vblank_irq,
		dev->drv->get_scanout_position);
}

/* ---- ioctls -------------------------------------------------------------- */

/* A 32-bit sequence from a client, taken as the 64-bit count nearest to
 * `near' with those low bits. */
static uint64_t widen_32_to_64(uint32_t narrow, uint64_t near)
{
	return near + (uint64_t)(int64_t)(int32_t)(narrow - (uint32_t)near);
}

/* A WAIT_VBLANK that only asks for the current count. */
static bool drm_wait_vblank_is_query(const union drm_wait_vblank *vblwait)
{
	if (vblwait->request.sequence)
		return false;
	return _DRM_VBLANK_RELATIVE ==
	       (vblwait->request.type & (_DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_EVENT |
					 _DRM_VBLANK_NEXTONMISS));
}

/* The count, and the time of its vblank, as WAIT_VBLANK reports them. */
static void drm_wait_vblank_reply(struct drm_device *dev, int pipe,
				  struct drm_wait_vblank_reply *reply)
{
	uint64_t now;

	reply->sequence = (uint32_t)drm_crtc_vblank_count_and_time(dev, pipe, &now);
	reply->tval_sec = (long)(uint32_t)(now / 1000000000ULL);
	reply->tval_usec = (long)((now % 1000000000ULL) / 1000);
}

/* WAIT_VBLANK with EVENT: the caller's reference either goes with the event
 * (a driver that switches the interrupt) or is dropped here. */
static int drm_queue_vblank_event(struct drm_device *dev, int pipe,
				  uint64_t req_seq, union drm_wait_vblank *vblwait,
				  struct drm_file *fp)
{
	struct drm_vblank_crtc *v = &dev->vbl[pipe];
	struct drm_pending_vblank_event *e;
	uint64_t seq = 0;
	int keep_ref = dev->drv->enable_vblank != NULL;

	e = drm_vblank_event_create(fp, DRM_EVENT_VBLANK, vblwait->request.signal);
	if (!e) {
		drm_crtc_vblank_put(dev, pipe);
		return -ENOMEM;
	}
	/* drm_crtc_vblank_off may have come since the reference was
	 * taken: no vblank will match */
	if (v->managed && !v->enabled) {
		kfree(e);
		drm_crtc_vblank_put(dev, pipe);
		return -EINVAL;
	}
	e->crtc = pipe;
	e->target = req_seq;
	e->holds_ref = keep_ref;
	if (vbl_queue(dev, e, &seq))
		vblwait->reply.sequence = (uint32_t)seq;
	else
		vblwait->reply.sequence = (uint32_t)req_seq;
	if (!keep_ref)
		drm_crtc_vblank_put(dev, pipe);
	return 0;
}

/* WAIT_VBLANK */
int drm_wait_vblank_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	union drm_wait_vblank *vblwait = kb;
	struct drm_vblank_crtc *v;
	uint32_t type = vblwait->request.type;
	uint32_t flags, high_pipe, pipe_index;
	uint64_t req_seq, seq;
	int pipe, ret = 0;

	if (!dev->ncrtc)
		return -EOPNOTSUPP;
	if (type & _DRM_VBLANK_SIGNAL)
		return -EINVAL;
	if (type & ~(_DRM_VBLANK_TYPES_MASK | _DRM_VBLANK_FLAGS_MASK |
		     _DRM_VBLANK_HIGH_CRTC_MASK))
		return -EINVAL;

	flags = type & _DRM_VBLANK_FLAGS_MASK;
	high_pipe = type & _DRM_VBLANK_HIGH_CRTC_MASK;
	if (high_pipe)
		pipe_index = high_pipe >> _DRM_VBLANK_HIGH_CRTC_SHIFT;
	else
		pipe_index = flags & _DRM_VBLANK_SECONDARY ? 1 : 0;
	/* No leases: a client's crtc index is the device's. */
	if (pipe_index >= dev->ncrtc)
		return -EINVAL;
	pipe = (int)pipe_index;
	v = &dev->vbl[pipe];

	/* A crtc whose driver does not switch vblank handling on and off
	 * counts while it is on and only then: off, no vblank comes. */
	if (!v->managed && !dev->crtc[pipe].active)
		return -EINVAL;

	/* An exact counter that is on answers a plain query from what it
	 * holds. */
	if (v->config.disable_immediate && drm_wait_vblank_is_query(vblwait) &&
	    v->enabled) {
		drm_wait_vblank_reply(dev, pipe, &vblwait->reply);
		return 0;
	}

	ret = drm_crtc_vblank_get(dev, pipe);
	if (ret)
		return ret;
	seq = drm_crtc_vblank_count(dev, pipe);

	switch (type & _DRM_VBLANK_TYPES_MASK) {
	case _DRM_VBLANK_RELATIVE:
		req_seq = seq + vblwait->request.sequence;
		/* written back absolute, so that a restarted call waits for
		 * the same vblank */
		vblwait->request.sequence = (unsigned int)req_seq;
		vblwait->request.type =
			(enum drm_vblank_seq_type)(vblwait->request.type & ~_DRM_VBLANK_RELATIVE);
		break;
	case _DRM_VBLANK_ABSOLUTE:
		req_seq = widen_32_to_64(vblwait->request.sequence, seq);
		break;
	default:
		ret = -EINVAL;
		goto done;
	}

	if ((flags & _DRM_VBLANK_NEXTONMISS) && drm_vblank_passed(seq, req_seq)) {
		req_seq = seq + 1;
		vblwait->request.type =
			(enum drm_vblank_seq_type)(vblwait->request.type & ~_DRM_VBLANK_NEXTONMISS);
		vblwait->request.sequence = (unsigned int)req_seq;
	}

	if (flags & _DRM_VBLANK_EVENT) {
		/* the reference goes with the event, or is dropped there */
		return drm_queue_vblank_event(dev, pipe, req_seq, vblwait, fp);
	}

	if (req_seq != seq)
		ret = vbl_wait_until(dev, pipe, req_seq, 3000, 1);

	if (ret != -EINTR)
		drm_wait_vblank_reply(dev, pipe, &vblwait->reply);
done:
	drm_crtc_vblank_put(dev, pipe);
	return ret;
}

/* CRTC_GET_SEQUENCE */
int drm_crtc_get_sequence_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp)
{
	(void)fp;
	struct drm_crtc_get_sequence *g = kb;
	struct drm_vblank_crtc *v;
	uint64_t now;
	int pipe, need_ref, ret;

	if (!dev->ncrtc)
		return -EOPNOTSUPP;
	struct drm_crtc *cr = drm_crtc_find(dev, g->crtc_id);
	if (!cr)
		return -ENOENT;
	pipe = cr->index;
	v = &dev->vbl[pipe];
	/* A reference brings the counter up to date where the interrupt
	 * may be off (and fails where the driver switched the crtc off);
	 * where the counter always counts there is nothing to bring. */
	need_ref = !(v->config.disable_immediate && v->enabled) &&
		   (dev->drv->enable_vblank || v->managed);
	if (need_ref) {
		ret = drm_crtc_vblank_get(dev, pipe);
		if (ret)
			return ret;
	}
	g->active = cr->active;
	g->sequence = drm_crtc_vblank_count_and_time(dev, pipe, &now);
	g->sequence_ns = (int64_t)now;
	if (need_ref)
		drm_crtc_vblank_put(dev, pipe);
	return 0;
}

/* CRTC_QUEUE_SEQUENCE */
int drm_crtc_queue_sequence_ioctl(struct drm_device *dev, void *kb,
				  struct drm_file *fp)
{
	struct drm_crtc_queue_sequence *q = kb;
	struct drm_pending_vblank_event *e;
	struct drm_vblank_crtc *v;
	uint64_t seq, req_seq, answered = 0;
	uint32_t flags;
	int pipe, ret, keep_ref;

	if (!dev->ncrtc)
		return -EOPNOTSUPP;
	struct drm_crtc *cr = drm_crtc_find(dev, q->crtc_id);
	if (!cr)
		return -ENOENT;
	flags = q->flags;
	if (flags & ~(DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS))
		return -EINVAL;
	pipe = cr->index;
	v = &dev->vbl[pipe];
	if (!v->managed && !cr->active)
		return -EINVAL;

	e = drm_vblank_event_create(fp, DRM_EVENT_CRTC_SEQUENCE, q->user_data);
	if (!e)
		return -ENOMEM;
	ret = drm_crtc_vblank_get(dev, pipe);
	if (ret) {
		kfree(e);
		return ret;
	}

	seq = drm_crtc_vblank_count(dev, pipe);
	req_seq = q->sequence;
	if (flags & DRM_CRTC_SEQUENCE_RELATIVE)
		req_seq += seq;
	if ((flags & DRM_CRTC_SEQUENCE_NEXT_ON_MISS) && drm_vblank_passed(seq, req_seq))
		req_seq = seq + 1;

	/* drm_crtc_vblank_off since the reference was taken */
	if (v->managed && !v->enabled) {
		drm_crtc_vblank_put(dev, pipe);
		kfree(e);
		return -EINVAL;
	}
	keep_ref = dev->drv->enable_vblank != NULL;
	e->crtc = pipe;
	e->target = req_seq;
	e->holds_ref = keep_ref;
	if (vbl_queue(dev, e, &answered))
		q->sequence = answered;
	else
		q->sequence = req_seq;
	if (!keep_ref)
		drm_crtc_vblank_put(dev, pipe);
	return 0;
}
