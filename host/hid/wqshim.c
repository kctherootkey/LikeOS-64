/* Work queue and string services for the HID harness (host/test-hid.sh).
 *
 * The HID core and hid-multitouch use the kernel's work queue API
 * (kernel/ke/workqueue.h): the LED output report and the sticky-finger
 * release run from work items.  The harness implements that API over a
 * simulated queue that the tests drive explicitly: queued items run when a
 * test calls test_wq_run(), delayed items when the harness clock
 * (timer_get_precise_ns, settable with test_set_clock_ns) has passed their
 * deadline at that moment.  That makes the 100 ms sticky-finger timeout
 * deterministic.  The guarantees the real implementation gives -- PENDING
 * semantics, cancel_*_sync waiting for a running instance, no concurrent
 * run of one item -- hold here too, also between real threads (the race
 * phase runs a worker thread that calls test_wq_run in a loop).
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/ke/workqueue.h>
#include <kernel/ke/timer.h>
#include <kernel/io/console.h>
#include "wqshim.h"

#define WQ_MAX_ITEMS 256

static volatile int g_wq_lock; /* raw lock: callable under spinlocks */
static struct work_struct *g_items[WQ_MAX_ITEMS]; /* queued or on a timer */
static int g_nitems;
static struct work_struct *volatile g_running[8];
static unsigned long g_runs;

struct workqueue_struct {
	int dummy;
};
static struct workqueue_struct g_system_wq;
struct workqueue_struct *system_wq = &g_system_wq;
struct workqueue_struct *system_highpri_wq = &g_system_wq;
struct workqueue_struct *system_long_wq = &g_system_wq;
struct workqueue_struct *system_unbound_wq = &g_system_wq;
struct workqueue_struct *system_freezable_wq = &g_system_wq;

static void wq_lock(void)
{
	while (__atomic_exchange_n(&g_wq_lock, 1, __ATOMIC_ACQUIRE))
		h_yield();
}

static void wq_unlock(void)
{
	__atomic_store_n(&g_wq_lock, 0, __ATOMIC_RELEASE);
}

static int wq_find(struct work_struct *w)
{
	for (int i = 0; i < g_nitems; i++)
		if (g_items[i] == w)
			return i;
	return -1;
}

static void wq_remove_at(int i)
{
	g_items[i] = g_items[--g_nitems];
}

static void wq_add(struct work_struct *w)
{
	if (g_nitems == WQ_MAX_ITEMS)
		test_fatal("work queue shim full", NULL);
	g_items[g_nitems++] = w;
}

void __init_work(struct work_struct *work, work_func_t func, int delayed)
{
	h_memset(work, 0, sizeof(*work));
	work->func = func;
	work->magic = WORK_INIT_MAGIC;
	work->is_delayed = (uint32_t)delayed;
}

void __init_delayed_work(struct delayed_work *dwork, work_func_t func)
{
	h_memset(dwork, 0, sizeof(*dwork));
	__init_work(&dwork->work, func, 1);
}

static void wq_check(struct work_struct *w)
{
	if (w->magic != WORK_INIT_MAGIC)
		test_fatal("work item used before INIT_WORK", NULL);
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	bool queued = false;

	wq_check(work);
	wq_lock();
	if (!(work->state & WORK_STATE_PENDING)) {
		work->state = WORK_STATE_PENDING | WORK_STATE_QUEUED;
		work->wq = wq;
		wq_add(work);
		queued = true;
	}
	wq_unlock();
	return queued;
}

bool queue_delayed_work(struct workqueue_struct *wq,
			struct delayed_work *dwork, unsigned long delay_ms)
{
	bool queued = false;

	wq_check(&dwork->work);
	wq_lock();
	if (!(dwork->work.state & WORK_STATE_PENDING)) {
		dwork->work.state = WORK_STATE_PENDING | WORK_STATE_TIMER;
		dwork->wq = wq;
		dwork->timer.expires_ns =
			timer_get_precise_ns() + delay_ms * 1000000ull;
		wq_add(&dwork->work);
		queued = true;
	}
	wq_unlock();
	return queued;
}

bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork,
		      unsigned long delay_ms)
{
	bool was_pending;
	int i;

	wq_check(&dwork->work);
	wq_lock();
	was_pending = (dwork->work.state & WORK_STATE_PENDING) != 0;
	if (dwork->work.state & WORK_STATE_CANCELING) {
		wq_unlock();
		return was_pending;
	}
	i = wq_find(&dwork->work);
	if (i < 0)
		wq_add(&dwork->work);
	dwork->work.state = WORK_STATE_PENDING | WORK_STATE_TIMER;
	dwork->wq = wq;
	dwork->timer.expires_ns =
		timer_get_precise_ns() + delay_ms * 1000000ull;
	wq_unlock();
	return was_pending;
}

static bool wq_cancel(struct work_struct *work)
{
	bool was_pending;
	int i;

	wq_lock();
	was_pending = (work->state & WORK_STATE_PENDING) != 0;
	i = wq_find(work);
	if (i >= 0)
		wq_remove_at(i);
	work->state = 0;
	wq_unlock();
	return was_pending;
}

bool cancel_work(struct work_struct *work)
{
	return wq_cancel(work);
}

bool cancel_delayed_work(struct delayed_work *dwork)
{
	return wq_cancel(&dwork->work);
}

static bool wq_is_running(struct work_struct *work)
{
	for (int i = 0; i < 8; i++)
		if (g_running[i] == work)
			return true;
	return false;
}

static bool wq_cancel_sync(struct work_struct *work)
{
	bool was;

	might_sleep();
	was = wq_cancel(work);
	while (wq_is_running(work))
		h_yield();
	return was;
}

bool cancel_work_sync(struct work_struct *work)
{
	return wq_cancel_sync(work);
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	return wq_cancel_sync(&dwork->work);
}

bool flush_work(struct work_struct *work)
{
	bool pending;

	might_sleep();
	wq_lock();
	pending = (work->state & WORK_STATE_PENDING) != 0;
	wq_unlock();
	if (pending)
		test_wq_run_until_idle();
	while (wq_is_running(work))
		h_yield();
	return pending;
}

bool flush_delayed_work(struct delayed_work *dwork)
{
	return flush_work(&dwork->work);
}

bool current_is_work(const struct work_struct *work)
{
	(void)work;
	return false;
}

bool current_is_workqueue_worker(void)
{
	return false;
}

/* Run what is due once; how many items ran. */
int test_wq_run(void)
{
	int ran = 0;

	for (;;) {
		struct work_struct *w = NULL;
		uint64_t now = timer_get_precise_ns();
		int slot = -1;

		wq_lock();
		for (int i = 0; i < g_nitems; i++) {
			struct work_struct *c = g_items[i];

			if (wq_is_running(c))
				continue; /* never concurrently with itself */
			if ((c->state & WORK_STATE_TIMER) &&
			    to_delayed_work(c)->timer.expires_ns > now)
				continue;
			w = c;
			wq_remove_at(i);
			break;
		}
		if (w) {
			for (int i = 0; i < 8; i++)
				if (!g_running[i]) {
					g_running[i] = w;
					slot = i;
					break;
				}
			if (slot < 0) {
				wq_add(w);
				w = NULL;
			} else {
				w->state = 0; /* PENDING cleared before it runs */
			}
		}
		wq_unlock();
		if (!w)
			break;
		w->func(w);
		__atomic_store_n(&g_running[slot], NULL, __ATOMIC_RELEASE);
		__atomic_fetch_add(&g_runs, 1, __ATOMIC_RELAXED);
		ran++;
	}
	return ran;
}

void test_wq_run_until_idle(void)
{
	while (test_wq_run())
		;
}

int test_wq_pending(void)
{
	int n;

	wq_lock();
	n = g_nitems;
	wq_unlock();
	return n;
}

unsigned long test_wq_runs(void)
{
	return __atomic_load_n(&g_runs, __ATOMIC_RELAXED);
}

/* ---- string helpers the input core harness does not provide ---- */

int kstrcmp(const char *s1, const char *s2)
{
	return h_strcmp(s1, s2);
}

int kstrncmp(const char *s1, const char *s2, size_t n)
{
	return h_strncmp(s1, s2, n);
}

int kmemcmp(const void *a, const void *b, size_t n)
{
	return h_memcmp(a, b, n);
}
