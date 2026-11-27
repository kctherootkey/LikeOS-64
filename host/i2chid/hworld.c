/* HID-world services of the I2C-HID harness (host/test-i2chid.sh), next
 * to the input harness's kstubs.c and the HID harness's wqshim.c: what
 * the I2C-HID transport and the console pointer bridge call that those
 * do not provide.
 *
 *   wait queues   wait_event_timeout() over a polling waiter (the
 *                 transport's reset wait): the condition is looked at
 *                 every 50 us until the deadline;
 *   delays        msleep / usleep_range on the host clock;
 *   work queue    a thread that runs the simulated queue continuously, so
 *                 work items run as they would on the real worker;
 *   mouse layer   mouse_inject_usb_movement() records every injection and
 *                 feeds the built-in mouse's event device (event1), as
 *                 mouse.c does; mouse_i2c_touchpad_report() runs the real
 *                 PS/2 preference state machine and records its verdict.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/ke/waitq.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/timer.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/dev/input/mouse.h>
#include <kernel/dev/input/mousedev.h>
#include "host/hid/wqshim.h"
#include "host/i2chid/hworld.h"

/* ---- wait queues ---- */

uint64_t hrtimer_now_ns(void)
{
	return (uint64_t)h_monotonic_ns();
}

uint64_t wq_deadline_after_ms(long ms)
{
	if (ms < 0)
		ms = 0;
	return hrtimer_now_ns() + (uint64_t)ms * 1000000ull;
}

int task_fatal_signal_pending(task_t *t)
{
	(void)t;
	return 0;
}

void wq_waiter_init(struct wq_waiter *w, struct wait_queue_head *h,
		    uint64_t deadline_ns, int mode)
{
	might_sleep();
	h_memset(w, 0, sizeof(*w));
	w->head = h;
	w->deadline_ns = deadline_ns;
	w->mode = mode;
}

void wq_waiter_prepare(struct wq_waiter *w)
{
	(void)w;
}

void wq_waiter_prepare_locked(struct wq_waiter *w)
{
	(void)w;
}

void wq_waiter_finish(struct wq_waiter *w)
{
	(void)w;
}

int wq_waiter_sleep(struct wq_waiter *w)
{
	might_sleep();
	if (w->deadline_ns && hrtimer_now_ns() >= w->deadline_ns)
		return 1;
	h_usleep(50);
	return 0;
}

/* The waiters above never queue themselves, so a wake finds nobody: these
 * are only there for the wake path the headers inline. */
int sched_claim_wake(task_t *t, task_state_t from)
{
	(void)t;
	(void)from;
	return 0;
}

void sched_enqueue_ready(task_t *task)
{
	(void)task;
}

/* ---- delays ---- */

void udelay(unsigned long usecs)
{
	h_usleep((unsigned int)usecs);
}

void mdelay(unsigned long msecs)
{
	h_usleep((unsigned int)(msecs * 1000));
}

void msleep(unsigned int msecs)
{
	might_sleep();
	h_usleep(msecs * 1000);
}

unsigned long msleep_interruptible(unsigned int msecs)
{
	msleep(msecs);
	return 0;
}

void usleep_range(unsigned long min_us, unsigned long max_us)
{
	(void)max_us;
	might_sleep();
	h_usleep((unsigned int)min_us);
}

void fsleep(unsigned long usecs)
{
	might_sleep();
	h_usleep((unsigned int)usecs);
}

int delay_can_sleep(void)
{
	return 1;
}

/* ---- the work queue worker ---- */

static volatile int g_wq_run;
static unsigned long g_wq_thread;

static void *wq_worker(void *p)
{
	(void)p;
	while (__atomic_load_n(&g_wq_run, __ATOMIC_ACQUIRE)) {
		if (!test_wq_run())
			h_usleep(200);
	}
	test_thread_done();
	return NULL;
}

void hw_wq_start(void)
{
	__atomic_store_n(&g_wq_run, 1, __ATOMIC_RELEASE);
	h_thread_start(wq_worker, NULL, &g_wq_thread);
}

void hw_wq_stop(void)
{
	__atomic_store_n(&g_wq_run, 0, __ATOMIC_RELEASE);
	h_thread_join(g_wq_thread);
}

/* ---- the mouse layer ---- */

static volatile int g_inj_lock;
static struct hw_injection g_inj[HW_MAX_INJ];
static volatile int g_ninj;

static void raw_lock(volatile int *l)
{
	while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
		h_yield();
}

static void raw_unlock(volatile int *l)
{
	__atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

void mouse_inject_usb_movement(int dx, int dy, uint8_t buttons, int8_t wheel)
{
	raw_lock(&g_inj_lock);
	if (g_ninj < HW_MAX_INJ) {
		g_inj[g_ninj].dx = dx;
		g_inj[g_ninj].dy = dy;
		g_inj[g_ninj].buttons = buttons;
		g_inj[g_ninj].wheel = wheel;
		g_ninj++;
	}
	raw_unlock(&g_inj_lock);
	/* What mouse.c does with it on the event-device side. */
	(void)evdev_feed_mouse(dx, dy, buttons & 0x07, (int)wheel, 0);
}

int hw_injections(struct hw_injection *out, int max)
{
	int n;

	raw_lock(&g_inj_lock);
	n = g_ninj;
	if (out) {
		if (n > max)
			n = max;
		h_memcpy(out, g_inj, (unsigned long)n * sizeof(*out));
	}
	raw_unlock(&g_inj_lock);
	return n;
}

void hw_injections_clear(void)
{
	raw_lock(&g_inj_lock);
	g_ninj = 0;
	raw_unlock(&g_inj_lock);
}

static volatile int g_pref_lock;
static struct mousedev_ps2_pref g_pref;
static int g_pref_verdicts, g_pref_reports;
static char g_pref_name[96];

void mouse_i2c_touchpad_report(const char *name, int contact)
{
	raw_lock(&g_pref_lock);
	g_pref_reports++;
	if (mousedev_ps2_pref_touchpad(&g_pref, contact) ==
	    MOUSEDEV_PS2_DISABLE) {
		g_pref_verdicts++;
		h_snprintf(g_pref_name, sizeof(g_pref_name), "%s",
			   name ? name : "?");
	}
	raw_unlock(&g_pref_lock);
}

void hw_pref_reset(int aux_present, int aux_trackpoint)
{
	raw_lock(&g_pref_lock);
	mousedev_ps2_pref_init(&g_pref, aux_present, aux_trackpoint);
	g_pref_verdicts = g_pref_reports = 0;
	g_pref_name[0] = 0;
	raw_unlock(&g_pref_lock);
}

int hw_pref_state(int *reports, char *name, int cap)
{
	int v;

	raw_lock(&g_pref_lock);
	v = g_pref_verdicts;
	if (reports)
		*reports = g_pref_reports;
	if (name)
		h_snprintf(name, (unsigned long)cap, "%s", g_pref_name);
	raw_unlock(&g_pref_lock);
	return v;
}
