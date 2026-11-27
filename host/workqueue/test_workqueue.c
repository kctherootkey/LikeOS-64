/* Host test of the generic work queues (kernel/ke/workqueue.c).
 *
 * The real workqueue.c runs on real threads (host/kshim): its workers are
 * pthreads, delayed items fire from a timer thread, and the tests race
 * queuers, cancellers and flushers from further threads.  Under
 * AddressSanitizer every "free after cancel_*_sync" below is a
 * use-after-free detector for a worker or timer callback that touches an
 * item after the cancel returned.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/ke/workqueue.h>
#include <kernel/ke/delay.h>

extern volatile unsigned long g_hrt_inflight_cancels;

/* ---- helpers --------------------------------------------------------------- */

struct counted {
	struct work_struct work;
	volatile int runs;
	volatile int running; /* reentrancy detector */
	volatile int max_running;
	volatile int requeue; /* queue itself again this many times */
	volatile unsigned long sleep_us;
	volatile unsigned long requested; /* queuer's sequence */
	volatile unsigned long seen; /* last sequence a run observed */
};

static void counted_fn(struct work_struct *w)
{
	struct counted *c = container_of(w, struct counted, work);
	int r = __atomic_add_fetch(&c->running, 1, __ATOMIC_SEQ_CST);

	if (r > c->max_running)
		c->max_running = r;
	T_ASSERT(!irqs_disabled());
	c->seen = __atomic_load_n(&c->requested, __ATOMIC_SEQ_CST);
	if (c->sleep_us)
		h_usleep(c->sleep_us);
	__atomic_add_fetch(&c->runs, 1, __ATOMIC_SEQ_CST);
	__atomic_sub_fetch(&c->running, 1, __ATOMIC_SEQ_CST);
	if (c->requeue > 0) {
		c->requeue--;
		(void)queue_work(system_wq, w);
	}
}

static void wait_until(volatile int *v, int target, unsigned int ms)
{
	uint64_t until = h_now_ns() + (uint64_t)ms * 1000000ull;

	while (__atomic_load_n(v, __ATOMIC_SEQ_CST) < target) {
		if (h_now_ns() > until)
			test_bug("timed out waiting", __FILE__, __LINE__,
				 __func__);
		h_usleep(100);
	}
}

/* ---- cases ----------------------------------------------------------------- */

static void t_basic(void)
{
	struct counted c = { 0 };

	INIT_WORK(&c.work, counted_fn);
	T_ASSERT(!work_pending(&c.work));
	T_ASSERT(queue_work(system_wq, &c.work));
	T_ASSERT(flush_work(&c.work) || c.runs == 1);
	T_EQ(c.runs, 1);
	T_ASSERT(!work_pending(&c.work));
	/* flush of an idle item: nothing to wait for */
	T_ASSERT(!flush_work(&c.work));
}

static void t_pending_coalesces(void)
{
	struct counted c = { 0 };
	int queued = 0;

	INIT_WORK(&c.work, counted_fn);
	c.sleep_us = 20000;
	/* First run occupies the item; then many queues while it runs
	 * collapse into one more run. */
	T_ASSERT(queue_work(system_wq, &c.work));
	wait_until(&c.running, 1, 1000);
	for (int i = 0; i < 100; i++)
		queued += queue_work(system_wq, &c.work) ? 1 : 0;
	T_EQ(queued, 1);
	(void)flush_work(&c.work);
	T_EQ(c.runs, 2);
	T_EQ(c.max_running, 1);
}

static void t_self_requeue(void)
{
	struct counted c = { 0 };

	INIT_WORK(&c.work, counted_fn);
	c.requeue = 50;
	T_ASSERT(queue_work(system_wq, &c.work));
	/* flush waits for the instance queued when it was called only */
	(void)flush_work(&c.work);
	wait_until(&c.runs, 51, 5000);
	(void)cancel_work_sync(&c.work);
	T_EQ(c.runs, 51);
	T_EQ(c.max_running, 1);
}

/* Many threads queue one item; no event may be lost and it never runs
 * on two workers at once. */
struct storm {
	struct counted *c;
	volatile int stop;
	volatile int done;
};

static void storm_thread(void *p)
{
	struct storm *s = p;

	for (int i = 0; i < 3000; i++) {
		uint64_t fl = local_irq_save(); /* as from an interrupt */

		__atomic_add_fetch(&s->c->requested, 1, __ATOMIC_SEQ_CST);
		(void)queue_work(system_wq, &s->c->work);
		local_irq_restore(fl);
		if ((h_rand() & 7) == 0)
			h_yield();
	}
	__atomic_add_fetch(&s->done, 1, __ATOMIC_SEQ_CST);
}

static void t_no_lost_events(void)
{
	struct counted c = { 0 };
	struct storm s = { &c, 0, 0 };

	INIT_WORK(&c.work, counted_fn);
	c.sleep_us = 5;
	for (int i = 0; i < 4; i++)
		T_ASSERT(sched_add_task(storm_thread, &s, NULL, 0));
	wait_until(&s.done, 4, 30000);
	(void)flush_work(&c.work);
	/* The run that followed the last queueing saw every request. */
	T_EQ(c.seen, 4 * 3000);
	T_EQ(c.max_running, 1);
}

/* cancel_work_sync while the item runs and requeues itself, then free:
 * nothing may touch it afterwards. */
static void t_cancel_sync_free(void)
{
	for (int round = 0; round < 200; round++) {
		struct counted *c = kcalloc(1, sizeof(*c));

		INIT_WORK(&c->work, counted_fn);
		c->requeue = 1000000;
		c->sleep_us = h_rand() % 50;
		(void)queue_work(system_wq, &c->work);
		h_usleep(h_rand() % 200);
		(void)cancel_work_sync(&c->work);
		T_ASSERT(!work_pending(&c->work));
		T_EQ(c->running, 0);
		/* queue_work refused while the cancel held it; it may be
		 * queued again now */
		T_ASSERT(queue_work(system_wq, &c->work));
		(void)cancel_work_sync(&c->work);
		kfree(c);
	}
}

/* Two cancellers and a queuer race on one item. */
struct cancel_race {
	struct counted *c;
	volatile int stop;
	volatile int done;
};

static void race_queuer(void *p)
{
	struct cancel_race *r = p;

	while (!__atomic_load_n(&r->stop, __ATOMIC_SEQ_CST)) {
		(void)queue_work(system_wq, &r->c->work);
		h_usleep(h_rand() % 30);
	}
	__atomic_add_fetch(&r->done, 1, __ATOMIC_SEQ_CST);
}

static void race_canceller(void *p)
{
	struct cancel_race *r = p;

	while (!__atomic_load_n(&r->stop, __ATOMIC_SEQ_CST)) {
		(void)cancel_work_sync(&r->c->work);
		(void)flush_work(&r->c->work);
		h_usleep(h_rand() % 30);
	}
	__atomic_add_fetch(&r->done, 1, __ATOMIC_SEQ_CST);
}

static void t_cancel_flush_race(void)
{
	struct counted *c = kcalloc(1, sizeof(*c));
	struct cancel_race r = { c, 0, 0 };

	INIT_WORK(&c->work, counted_fn);
	c->sleep_us = 10;
	T_ASSERT(sched_add_task(race_queuer, &r, NULL, 0));
	T_ASSERT(sched_add_task(race_queuer, &r, NULL, 0));
	T_ASSERT(sched_add_task(race_canceller, &r, NULL, 0));
	T_ASSERT(sched_add_task(race_canceller, &r, NULL, 0));
	h_usleep(800000);
	__atomic_store_n(&r.stop, 1, __ATOMIC_SEQ_CST);
	wait_until(&r.done, 4, 10000);
	(void)cancel_work_sync(&c->work);
	T_EQ(c->max_running, 1);
	T_EQ(c->running, 0);
	kfree(c);
}

/* Delayed items: deadline, mod_delayed_work, cancel with a timer
 * callback in flight. */
struct dcounted {
	struct delayed_work dw;
	volatile int runs;
	volatile uint64_t ran_at;
};

static void dcounted_fn(struct work_struct *w)
{
	struct dcounted *d = container_of(to_delayed_work(w), struct dcounted,
					  dw);

	d->ran_at = h_now_ns();
	__atomic_add_fetch(&d->runs, 1, __ATOMIC_SEQ_CST);
}

static void t_delayed_basic(void)
{
	struct dcounted d = { 0 };
	uint64_t t0;

	INIT_DELAYED_WORK(&d.dw, dcounted_fn);
	t0 = h_now_ns();
	T_ASSERT(queue_delayed_work(system_wq, &d.dw, 30));
	T_ASSERT(!queue_delayed_work(system_wq, &d.dw, 1));
	wait_until(&d.runs, 1, 2000);
	T_ASSERT(d.ran_at - t0 >= 30ull * 1000000ull);
	/* mod_delayed_work moves an armed deadline */
	T_ASSERT(!mod_delayed_work(system_wq, &d.dw, 200));
	T_ASSERT(mod_delayed_work(system_wq, &d.dw, 5));
	wait_until(&d.runs, 2, 2000);
	/* flush_delayed_work runs it now */
	T_ASSERT(queue_delayed_work(system_wq, &d.dw, 100000));
	t0 = h_now_ns();
	/* (false when a worker ran it before the flush looked) */
	(void)flush_delayed_work(&d.dw);
	T_EQ(d.runs, 3);
	T_ASSERT(h_now_ns() - t0 < 5000ull * 1000000ull);
	/* cancel of an armed one: it never runs */
	T_ASSERT(queue_delayed_work(system_wq, &d.dw, 50));
	T_ASSERT(cancel_delayed_work_sync(&d.dw));
	h_usleep(100000);
	T_EQ(d.runs, 3);
	T_ASSERT(!delayed_work_pending(&d.dw));
}

static void t_delayed_cancel_free(void)
{
	/* Deadlines of 0-1 ms put the timer callback right into the
	 * cancel's window; the item is freed straight after the cancel. */
	for (int round = 0; round < 2000; round++) {
		struct dcounted *d = kcalloc(1, sizeof(*d));

		INIT_DELAYED_WORK(&d->dw, dcounted_fn);
		(void)queue_delayed_work(system_wq, &d->dw, round & 1);
		if (h_rand() & 1)
			h_usleep(h_rand() % 1500);
		if (h_rand() & 1)
			(void)mod_delayed_work(system_wq, &d->dw, h_rand() & 1);
		(void)cancel_delayed_work_sync(&d->dw);
		T_ASSERT(!delayed_work_pending(&d->dw));
		kfree(d);
	}
}

/* Timer callbacks re-arm an item from "interrupt" context while a
 * thread mods and cancels it. */
struct dstorm {
	struct dcounted *d;
	volatile int stop, done;
};

static void dstorm_modder(void *p)
{
	struct dstorm *s = p;

	while (!__atomic_load_n(&s->stop, __ATOMIC_SEQ_CST)) {
		uint64_t fl = local_irq_save();

		(void)mod_delayed_work(system_wq, &s->d->dw, h_rand() % 2);
		local_irq_restore(fl);
		h_usleep(h_rand() % 100);
	}
	__atomic_add_fetch(&s->done, 1, __ATOMIC_SEQ_CST);
}

static void t_delayed_mod_storm(void)
{
	struct dcounted *d = kcalloc(1, sizeof(*d));
	struct dstorm s = { d, 0, 0 };

	INIT_DELAYED_WORK(&d->dw, dcounted_fn);
	for (int i = 0; i < 3; i++)
		T_ASSERT(sched_add_task(dstorm_modder, &s, NULL, 0));
	for (int i = 0; i < 300; i++) {
		(void)cancel_delayed_work_sync(&d->dw);
		h_usleep(h_rand() % 300);
	}
	__atomic_store_n(&s.stop, 1, __ATOMIC_SEQ_CST);
	wait_until(&s.done, 3, 10000);
	(void)cancel_delayed_work_sync(&d->dw);
	kfree(d);
}

/* A function that frees its own item: the worker must not touch it. */
static volatile int g_selffree_runs;

static void selffree_fn(struct work_struct *w)
{
	kfree(w);
	__atomic_add_fetch(&g_selffree_runs, 1, __ATOMIC_SEQ_CST);
}

static void t_self_free(void)
{
	for (int i = 0; i < 500; i++) {
		struct work_struct *w = kcalloc(1, sizeof(*w));

		INIT_WORK(w, selffree_fn);
		T_ASSERT(queue_work(system_wq, w));
	}
	wait_until(&g_selffree_runs, 500, 10000);
}

/* Ordered queue: strictly FIFO, one at a time. */
static volatile int g_order[64];
static volatile int g_order_n;

struct ordered_item {
	struct work_struct work;
	int idx;
};

static void ordered_fn(struct work_struct *w)
{
	struct ordered_item *o = container_of(w, struct ordered_item, work);
	int n = __atomic_fetch_add(&g_order_n, 1, __ATOMIC_SEQ_CST);

	g_order[n] = o->idx;
	h_usleep(h_rand() % 200);
}

static void t_ordered_and_destroy(void)
{
	struct workqueue_struct *wq = alloc_ordered_workqueue("test_ord", 0);
	struct ordered_item items[64];

	T_ASSERT(wq != NULL);
	for (int i = 0; i < 64; i++) {
		items[i].idx = i;
		INIT_WORK(&items[i].work, ordered_fn);
		T_ASSERT(queue_work(wq, &items[i].work));
	}
	flush_workqueue(wq);
	T_EQ(g_order_n, 64);
	for (int i = 0; i < 64; i++)
		T_EQ(g_order[i], i);
	/* destroy with items still queued: they run first */
	g_order_n = 0;
	for (int i = 0; i < 16; i++)
		T_ASSERT(queue_work(wq, &items[i].work));
	destroy_workqueue(wq);
	T_EQ(g_order_n, 16);
}

/* flush_workqueue waits for what was queued before, not after. */
static void t_flush_workqueue(void)
{
	struct workqueue_struct *wq = alloc_workqueue("test_flush", 0, 3);
	struct counted c[8];

	T_ASSERT(wq != NULL);
	for (int i = 0; i < 8; i++) {
		kmemset(&c[i], 0, sizeof(c[i]));
		INIT_WORK(&c[i].work, counted_fn);
		c[i].sleep_us = 2000 + (h_rand() % 3000);
		T_ASSERT(queue_work(wq, &c[i].work));
	}
	flush_workqueue(wq);
	for (int i = 0; i < 8; i++)
		T_EQ(c[i].runs, 1);
	destroy_workqueue(wq);
}

/* cancel from inside the function is refused (would deadlock). */
static void selfcancel_fn(struct work_struct *w)
{
	T_ASSERT(!cancel_work_sync(w));
	T_ASSERT(!flush_work(w));
}

static void t_self_cancel_refused(void)
{
	struct work_struct w;
	int warn0 = g_test_warnings;

	INIT_WORK(&w, selfcancel_fn);
	T_ASSERT(queue_work(system_wq, &w));
	h_usleep(50000);
	(void)cancel_work_sync(&w);
	/* both misuses were reported */
	T_EQ(g_test_warnings - warn0, 2);
	g_test_warnings = warn0;
}

int main(void);
int main(void)
{
	kshim_init_main();
	workqueue_init();
	T_ASSERT(system_wq && system_highpri_wq && system_long_wq);
	test_case("basic", t_basic);
	test_case("pending_coalesces", t_pending_coalesces);
	test_case("self_requeue", t_self_requeue);
	test_case("no_lost_events", t_no_lost_events);
	test_case("cancel_sync_free", t_cancel_sync_free);
	test_case("cancel_flush_race", t_cancel_flush_race);
	test_case("delayed_basic", t_delayed_basic);
	test_case("delayed_cancel_free", t_delayed_cancel_free);
	test_case("delayed_mod_storm", t_delayed_mod_storm);
	test_case("self_free", t_self_free);
	test_case("ordered_and_destroy", t_ordered_and_destroy);
	test_case("flush_workqueue", t_flush_workqueue);
	test_case("self_cancel_refused", t_self_cancel_refused);
	kprintf("timer callbacks met in flight by a cancel: %lu\n",
		g_hrt_inflight_cancels);
	test_summary_and_exit();
	return 0;
}
