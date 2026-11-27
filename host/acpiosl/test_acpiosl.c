/* Host test of the ACPICA OS layer's threading part
 * (kernel/hal/acpica/oslikeos_sync.c): mutexes and semaphores with their
 * timeout rules, in task, interrupts-off and before-the-scheduler
 * contexts; spinlocks; thread ids; the deferred execution of
 * AcpiOsExecute (boot list, ordered queues, FIFO order, never inline in
 * the caller) and AcpiOsWaitEventsComplete; the polled global lock wait;
 * sleep, stall and the timer.  Real work queues (kernel/ke/workqueue.c)
 * on real threads (host/kshim).
 *
 * Copyright (C) 2026 The LikeOS Project */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "kernel/hal/acpica/include/acpi.h"
#pragma GCC diagnostic pop

#include <kernel/ke/completion.h>
#include <kernel/ke/workqueue.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/hrtimer.h>

/* The global lock's semaphore (acglobal.h declares it). */
ACPI_SEMAPHORE AcpiGbl_GlobalLockSemaphore;

void acpi_os_run_boot_deferred(void);
int acpi_os_initialize1(void);
int acpi_os_deferral_live(void);

/* ==========================================================================
 * Completions (the kernel's are built on its wait queues)
 * ========================================================================== */

void init_completion(struct completion *x)
{
	__atomic_store_n(&x->done, 0, __ATOMIC_RELEASE);
	wq_head_init(&x->wait, "completion");
}

void complete(struct completion *x)
{
	__atomic_add_fetch(&x->done, 1, __ATOMIC_ACQ_REL);
	h_wq_wake();
}

int try_wait_for_completion(struct completion *x)
{
	uint32_t d = __atomic_load_n(&x->done, __ATOMIC_ACQUIRE);

	while (d) {
		if (__atomic_compare_exchange_n(&x->done, &d, d - 1, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			return 1;
	}
	return 0;
}

int completion_done(struct completion *x)
{
	return __atomic_load_n(&x->done, __ATOMIC_ACQUIRE) != 0;
}

long wait_for_completion_timeout(struct completion *x, long timeout_ms)
{
	uint64_t deadline = h_now_ns() + (uint64_t)timeout_ms * 1000000ull;

	might_sleep();
	for (;;) {
		uint64_t now;

		if (try_wait_for_completion(x)) {
			now = h_now_ns();
			return now >= deadline ?
				       1 :
				       (long)((deadline - now) / 1000000ull) + 1;
		}
		if (h_now_ns() >= deadline)
			return 0;
		h_usleep(20);
	}
}

void wait_for_completion(struct completion *x)
{
	while (!wait_for_completion_timeout(x, 1000))
		;
}

/* The SCI handler's in-flight wait (oslikeos.c): counted. */
static volatile int g_sci_syncs;
void acpi_osl_sci_sync(void);
void acpi_osl_sci_sync(void)
{
	__atomic_add_fetch(&g_sci_syncs, 1, __ATOMIC_SEQ_CST);
}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static uint64_t ms_since(uint64_t t0)
{
	return (h_now_ns() - t0) / 1000000ull;
}

static void wait_flag(volatile int *v, int target, unsigned int ms)
{
	uint64_t until = h_now_ns() + (uint64_t)ms * 1000000ull;

	while (__atomic_load_n(v, __ATOMIC_SEQ_CST) < target) {
		if (h_now_ns() > until)
			test_bug("timed out waiting", __FILE__, __LINE__,
				 __func__);
		h_usleep(100);
	}
}

static void join_task(task_t *t)
{
	while (!__atomic_load_n(&t->gone, __ATOMIC_ACQUIRE))
		h_usleep(100);
}

/* A thread that is no task: "before the scheduler" (and interrupts off,
 * the thread's flag starts clear). */
struct raw_ctx {
	void (*fn)(void *);
	void *arg;
	volatile int done;
};

static void raw_tramp(void *p)
{
	struct raw_ctx *r = p;

	r->fn(r->arg);
	__atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
	h_wq_wake();
}

static void run_raw(void (*fn)(void *), void *arg)
{
	struct raw_ctx r = { fn, arg, 0 };

	T_EQ(h_thread_create(raw_tramp, &r), 0);
	while (!__atomic_load_n(&r.done, __ATOMIC_ACQUIRE))
		h_usleep(100);
}

/* ==========================================================================
 * Mutexes
 * ========================================================================== */

struct mx_ctx {
	ACPI_MUTEX m;
	UINT16 timeout;
	ACPI_STATUS st;
	uint64_t waited_ms;
	int irqs_off;
	int release_after;
	volatile int started;
};

static void mx_waiter(void *p)
{
	struct mx_ctx *c = p;
	uint64_t t0, f = 0;

	if (c->irqs_off)
		f = local_irq_save();
	__atomic_store_n(&c->started, 1, __ATOMIC_SEQ_CST);
	t0 = h_now_ns();
	c->st = AcpiOsAcquireMutex(c->m, c->timeout);
	c->waited_ms = ms_since(t0);
	if (c->irqs_off)
		local_irq_restore(f);
	if (c->st == AE_OK && c->release_after)
		AcpiOsReleaseMutex(c->m);
}

static void t_mutex(void)
{
	ACPI_MUTEX m = NULL;
	struct mx_ctx c;
	task_t *t;
	uint64_t t0;
	int w0;

	T_EQ(AcpiOsCreateMutex(&m), AE_OK);
	T_ASSERT(m != NULL);
	T_EQ(AcpiOsCreateMutex(NULL), AE_BAD_PARAMETER);

	/* 0: try once */
	T_EQ(AcpiOsAcquireMutex(m, ACPI_DO_NOT_WAIT), AE_OK);
	T_EQ(AcpiOsAcquireMutex(m, ACPI_DO_NOT_WAIT), AE_TIME);
	/* a timed wait waits its time, then fails */
	t0 = h_now_ns();
	T_EQ(AcpiOsAcquireMutex(m, 40), AE_TIME);
	T_ASSERT(ms_since(t0) >= 39);

	/* forever: another task blocks until the release */
	kmemset(&c, 0, sizeof(c));
	c.m = m;
	c.timeout = ACPI_WAIT_FOREVER;
	c.release_after = 1;
	t = sched_add_task(mx_waiter, &c, NULL, 0);
	wait_flag(&c.started, 1, 2000);
	h_usleep(30000);
	T_EQ(__atomic_load_n(&t->gone, __ATOMIC_ACQUIRE), 0);
	AcpiOsReleaseMutex(m);
	join_task(t);
	T_EQ(c.st, AE_OK);
	T_ASSERT(c.waited_ms >= 25);

	/* a timed wait in another task that gets the mutex in time */
	T_EQ(AcpiOsAcquireMutex(m, ACPI_WAIT_FOREVER), AE_OK);
	kmemset(&c, 0, sizeof(c));
	c.m = m;
	c.timeout = 2000;
	c.release_after = 1;
	t = sched_add_task(mx_waiter, &c, NULL, 0);
	wait_flag(&c.started, 1, 2000);
	h_usleep(20000);
	AcpiOsReleaseMutex(m);
	join_task(t);
	T_EQ(c.st, AE_OK);
	T_ASSERT(c.waited_ms < 1500);

	/* released by another thread than the one that took it (ACPICA's
	 * namespace reader lock does that): free again afterwards */
	T_EQ(AcpiOsAcquireMutex(m, 0), AE_OK);
	kmemset(&c, 0, sizeof(c));
	c.m = m;
	c.timeout = ACPI_WAIT_FOREVER;
	c.release_after = 1;
	t = sched_add_task(mx_waiter, &c, NULL, 0);
	wait_flag(&c.started, 1, 2000);
	AcpiOsReleaseMutex(m);
	join_task(t);
	T_EQ(AcpiOsAcquireMutex(m, 0), AE_OK);
	AcpiOsReleaseMutex(m);

	/* releasing a free mutex is refused (two holders later) */
	w0 = g_test_warnings;
	AcpiOsReleaseMutex(m);
	T_EQ(g_test_warnings - w0, 1);
	g_test_warnings = w0;
	T_EQ(AcpiOsAcquireMutex(m, 0), AE_OK);
	T_EQ(AcpiOsAcquireMutex(m, 0), AE_TIME);
	AcpiOsReleaseMutex(m);
	AcpiOsDeleteMutex(m);
}

/* Interrupts off, scheduler running: polled, with the timeout honoured. */
static void t_mutex_irqs_off(void)
{
	ACPI_MUTEX m = NULL;
	struct mx_ctx c;
	task_t *t;

	T_EQ(AcpiOsCreateMutex(&m), AE_OK);
	T_EQ(AcpiOsAcquireMutex(m, 0), AE_OK);
	/* gets it once the holder lets go */
	kmemset(&c, 0, sizeof(c));
	c.m = m;
	c.timeout = ACPI_WAIT_FOREVER;
	c.irqs_off = 1;
	c.release_after = 1;
	t = sched_add_task(mx_waiter, &c, NULL, 0);
	wait_flag(&c.started, 1, 2000);
	h_usleep(20000);
	AcpiOsReleaseMutex(m);
	join_task(t);
	T_EQ(c.st, AE_OK);
	/* a timed poll that runs out */
	T_EQ(AcpiOsAcquireMutex(m, 0), AE_OK);
	kmemset(&c, 0, sizeof(c));
	c.m = m;
	c.timeout = 15;
	c.irqs_off = 1;
	t = sched_add_task(mx_waiter, &c, NULL, 0);
	join_task(t);
	T_EQ(c.st, AE_TIME);
	T_ASSERT(c.waited_ms >= 14);
	AcpiOsReleaseMutex(m);
	AcpiOsDeleteMutex(m);
}

/* Before the scheduler: one thread, nobody can release. */
struct boot_ctx {
	ACPI_MUTEX m;
	ACPI_STATUS st_free, st_try, st_timed, st_forever;
	uint64_t timed_ms;
	ACPI_THREAD_ID tid;
};

static void boot_fn(void *p)
{
	struct boot_ctx *b = p;
	uint64_t t0;

	b->tid = AcpiOsGetThreadId();
	b->st_free = AcpiOsAcquireMutex(b->m, ACPI_WAIT_FOREVER);
	b->st_try = AcpiOsAcquireMutex(b->m, 0);
	t0 = h_now_ns();
	b->st_timed = AcpiOsAcquireMutex(b->m, 25);
	b->timed_ms = ms_since(t0);
	b->st_forever = AcpiOsAcquireMutex(b->m, ACPI_WAIT_FOREVER);
	AcpiOsReleaseMutex(b->m);
}

static void t_before_scheduler(void)
{
	struct boot_ctx b;
	int w0 = g_test_warnings;

	kmemset(&b, 0, sizeof(b));
	T_EQ(AcpiOsCreateMutex(&b.m), AE_OK);
	run_raw(boot_fn, &b);
	T_EQ(b.tid, 1);
	T_EQ(b.st_free, AE_OK); /* a free mutex is simply taken */
	T_EQ(b.st_try, AE_TIME);
	T_EQ(b.st_timed, AE_TIME);
	T_ASSERT(b.timed_ms >= 24); /* a finite wait is waited out */
	T_EQ(b.st_forever, AE_TIME); /* never forever */
	T_EQ(g_test_warnings - w0, 1);
	g_test_warnings = w0;
	T_EQ(AcpiOsAcquireMutex(b.m, 0), AE_OK);
	AcpiOsReleaseMutex(b.m);
	AcpiOsDeleteMutex(b.m);
}

/* ==========================================================================
 * Semaphores
 * ========================================================================== */

struct sig_ctx {
	ACPI_SEMAPHORE s;
	int n;
	unsigned int delay_us;
};

/* Signals with interrupts off, as the SCI handler does. */
static void sig_fn(void *p)
{
	struct sig_ctx *c = p;
	uint64_t f;

	h_usleep(c->delay_us);
	f = local_irq_save();
	for (int i = 0; i < c->n; i++)
		T_EQ(AcpiOsSignalSemaphore(c->s, 1), AE_OK);
	local_irq_restore(f);
}

static void t_semaphore(void)
{
	ACPI_SEMAPHORE s = NULL;
	struct sig_ctx c;
	task_t *t;
	uint64_t t0;

	T_EQ(AcpiOsCreateSemaphore(ACPI_NO_UNIT_LIMIT, 2, &s), AE_OK);
	T_EQ(AcpiOsWaitSemaphore(s, 1, 0), AE_OK);
	T_EQ(AcpiOsWaitSemaphore(s, 1, ACPI_WAIT_FOREVER), AE_OK);
	T_EQ(AcpiOsWaitSemaphore(s, 1, 0), AE_TIME);
	T_EQ(AcpiOsWaitSemaphore(s, 2, 0), AE_SUPPORT);
	T_EQ(AcpiOsWaitSemaphore(s, 0, 0), AE_BAD_PARAMETER);
	T_EQ(AcpiOsSignalSemaphore(s, 0), AE_BAD_PARAMETER);
	T_EQ(AcpiOsWaitSemaphore(NULL, 1, 0), AE_BAD_PARAMETER);

	/* counting: three units signalled, three waits succeed */
	for (int i = 0; i < 3; i++)
		T_EQ(AcpiOsSignalSemaphore(s, 1), AE_OK);
	for (int i = 0; i < 3; i++)
		T_EQ(AcpiOsWaitSemaphore(s, 1, 0), AE_OK);
	T_EQ(AcpiOsWaitSemaphore(s, 1, 0), AE_TIME);

	/* a waiter woken by a signal from "interrupt context" */
	c.s = s;
	c.n = 1;
	c.delay_us = 20000;
	t = sched_add_task(sig_fn, &c, NULL, 0);
	t0 = h_now_ns();
	T_EQ(AcpiOsWaitSemaphore(s, 1, 3000), AE_OK);
	T_ASSERT(ms_since(t0) >= 15 && ms_since(t0) < 2500);
	join_task(t);
	/* AML Wait() with a timeout that passes */
	t0 = h_now_ns();
	T_EQ(AcpiOsWaitSemaphore(s, 1, 30), AE_TIME);
	T_ASSERT(ms_since(t0) >= 29);
	T_EQ(AcpiOsDeleteSemaphore(s), AE_OK);
}

/* The global lock: no release interrupt comes, the wait is a short poll
 * after which ACPICA retries the hardware lock. */
static void t_global_lock_poll(void)
{
	ACPI_SEMAPHORE s = NULL;
	uint64_t t0;

	T_EQ(AcpiOsCreateSemaphore(1, 0, &s), AE_OK);
	AcpiGbl_GlobalLockSemaphore = s;
	t0 = h_now_ns();
	for (int i = 0; i < 5; i++)
		T_EQ(AcpiOsWaitSemaphore(s, 1, ACPI_WAIT_FOREVER), AE_OK);
	T_ASSERT(ms_since(t0) < 1000);
	/* a signal is still taken */
	T_EQ(AcpiOsSignalSemaphore(s, 1), AE_OK);
	T_EQ(AcpiOsWaitSemaphore(s, 1, ACPI_WAIT_FOREVER), AE_OK);
	T_EQ(completion_done((struct completion *)s), 0);
	/* other timeouts are not polled */
	T_EQ(AcpiOsWaitSemaphore(s, 1, 0), AE_TIME);
	AcpiGbl_GlobalLockSemaphore = NULL;
	T_EQ(AcpiOsDeleteSemaphore(s), AE_OK);
}

/* ==========================================================================
 * Spinlocks, thread ids
 * ========================================================================== */

static ACPI_THREAD_ID g_tid_other;

static void tid_fn(void *p)
{
	(void)p;
	g_tid_other = AcpiOsGetThreadId();
}

static void t_lock_and_ids(void)
{
	ACPI_SPINLOCK l = NULL;
	ACPI_CPU_FLAGS f;
	ACPI_THREAD_ID me = AcpiOsGetThreadId();
	task_t *t;

	T_EQ(AcpiOsCreateLock(&l), AE_OK);
	T_ASSERT(!irqs_disabled());
	f = AcpiOsAcquireLock(l);
	T_ASSERT(irqs_disabled());
	T_ASSERT(spin_is_locked((spinlock_t *)l));
	AcpiOsReleaseLock(l, f);
	T_ASSERT(!irqs_disabled());
	/* nested in an interrupts-off section: stays off */
	{
		uint64_t outer = local_irq_save();

		f = AcpiOsAcquireLock(l);
		AcpiOsReleaseLock(l, f);
		T_ASSERT(irqs_disabled());
		local_irq_restore(outer);
	}
	AcpiOsDeleteLock(l);

	T_ASSERT(me != 0);
	T_ASSERT(me != (ACPI_THREAD_ID)-1);
	T_EQ(me, AcpiOsGetThreadId());
	t = sched_add_task(tid_fn, NULL, NULL, 0);
	join_task(t);
	T_ASSERT(g_tid_other != 0 && g_tid_other != me);
}

/* ==========================================================================
 * Deferred execution
 * ========================================================================== */

#define LOG_MAX 64
static volatile int g_log[LOG_MAX];
static volatile int g_nlog;
static spinlock_t g_log_lock = SPINLOCK_INIT("log");
static task_t *volatile g_runner[LOG_MAX];
static volatile int g_irqs_off_seen;
static unsigned int g_cb_sleep_us;

static void log_cb(void *ctx)
{
	uint64_t f;
	int i;

	if (irqs_disabled())
		__atomic_store_n(&g_irqs_off_seen, 1, __ATOMIC_SEQ_CST);
	if (g_cb_sleep_us)
		h_usleep(g_cb_sleep_us);
	spin_lock_irqsave(&g_log_lock, &f);
	i = g_nlog;
	if (i < LOG_MAX) {
		g_log[i] = (int)(unsigned long)ctx;
		g_runner[i] = sched_current();
	}
	g_nlog = i + 1;
	spin_unlock_irqrestore(&g_log_lock, f);
	h_wq_wake();
}

static void log_reset(void)
{
	g_nlog = 0;
	g_irqs_off_seen = 0;
	g_cb_sleep_us = 0;
}

/* Before the queues: kept, run by the boot poll in order, or queued by
 * the initialisation ahead of anything new. */
static void t_execute_boot(void)
{
	log_reset();
	T_EQ(acpi_os_deferral_live(), 0);
	T_EQ(AcpiOsExecute(OSL_GPE_HANDLER, log_cb, (void *)1), AE_OK);
	T_EQ(AcpiOsExecute(OSL_NOTIFY_HANDLER, log_cb, (void *)2), AE_OK);
	T_EQ(AcpiOsExecute(OSL_GPE_HANDLER, log_cb, (void *)3), AE_OK);
	T_EQ(g_nlog, 0); /* never in the caller */
	acpi_os_run_boot_deferred();
	T_EQ(g_nlog, 3);
	T_EQ(g_log[0], 1);
	T_EQ(g_log[1], 2);
	T_EQ(g_log[2], 3);
	T_EQ(AcpiOsExecute(OSL_DEBUGGER_MAIN_THREAD, log_cb, NULL), AE_ERROR);
	T_EQ(AcpiOsExecute(OSL_GPE_HANDLER, NULL, NULL), AE_BAD_PARAMETER);

	/* left over at initialisation: queued first, in order */
	log_reset();
	for (int i = 0; i < 5; i++)
		T_EQ(AcpiOsExecute(OSL_GPE_HANDLER, log_cb,
				   (void *)(unsigned long)(10 + i)),
		     AE_OK);
	T_EQ(acpi_os_initialize1(), 0);
	T_EQ(acpi_os_deferral_live(), 1);
	T_EQ(AcpiOsExecute(OSL_GPE_HANDLER, log_cb, (void *)15), AE_OK);
	wait_flag(&g_nlog, 6, 5000);
	for (int i = 0; i < 6; i++)
		T_EQ(g_log[i], 10 + i);
	for (int i = 0; i < 6; i++)
		T_ASSERT(g_runner[i] != sched_current());
	/* the boot poll has nothing to do any more */
	acpi_os_run_boot_deferred();
	T_EQ(g_nlog, 6);
}

struct irq_exec {
	int n;
	volatile int returned;
	volatile int nlog_at_return;
};

/* Queued from "interrupt context": returns at once, runs later in a
 * worker with interrupts on. */
static void irq_exec_fn(void *p)
{
	struct irq_exec *e = p;
	uint64_t f = local_irq_save();

	for (int i = 0; i < e->n; i++)
		T_EQ(AcpiOsExecute(OSL_GPE_HANDLER, log_cb,
				   (void *)(unsigned long)(100 + i)),
		     AE_OK);
	e->nlog_at_return = g_nlog;
	local_irq_restore(f);
	__atomic_store_n(&e->returned, 1, __ATOMIC_SEQ_CST);
}

static void t_execute_live(void)
{
	struct irq_exec e = { 20, 0, 0 };
	task_t *t;
	int s0;

	log_reset();
	g_cb_sleep_us = 2000;
	t = sched_add_task(irq_exec_fn, &e, NULL, 0);
	join_task(t);
	T_ASSERT(e.nlog_at_return < 20);
	wait_flag(&g_nlog, 20, 10000);
	for (int i = 0; i < 20; i++)
		T_EQ(g_log[i], 100 + i); /* FIFO: an ordered queue */
	T_EQ(g_irqs_off_seen, 0);
	/* one worker: the same task ran them all */
	for (int i = 1; i < 20; i++)
		T_ASSERT(g_runner[i] == g_runner[0]);

	/* notify and GPE run on different queues */
	log_reset();
	T_EQ(AcpiOsExecute(OSL_GPE_HANDLER, log_cb, (void *)1), AE_OK);
	T_EQ(AcpiOsExecute(OSL_NOTIFY_HANDLER, log_cb, (void *)2), AE_OK);
	wait_flag(&g_nlog, 2, 5000);
	T_ASSERT(g_runner[0] != g_runner[1]);

	/* AcpiOsWaitEventsComplete: everything queued before has run */
	log_reset();
	g_cb_sleep_us = 20000;
	s0 = g_sci_syncs;
	for (int i = 0; i < 3; i++) {
		T_EQ(AcpiOsExecute(OSL_GPE_HANDLER, log_cb, (void *)7), AE_OK);
		T_EQ(AcpiOsExecute(OSL_NOTIFY_HANDLER, log_cb, (void *)8),
		     AE_OK);
	}
	AcpiOsWaitEventsComplete();
	T_EQ(g_nlog, 6);
	T_ASSERT(g_sci_syncs > s0);
	g_cb_sleep_us = 0;
}

/* AcpiOsWaitEventsComplete from a deferred call itself (a Notify handler
 * removing a handler): no wait for itself. */
static volatile int g_inner_done;

static void inner_cb(void *ctx)
{
	(void)ctx;
	AcpiOsWaitEventsComplete();
	__atomic_store_n(&g_inner_done, 1, __ATOMIC_SEQ_CST);
}

static void t_wait_from_worker(void)
{
	g_inner_done = 0;
	T_EQ(AcpiOsExecute(OSL_NOTIFY_HANDLER, inner_cb, NULL), AE_OK);
	wait_flag(&g_inner_done, 1, 5000);
	g_inner_done = 0;
	T_EQ(AcpiOsExecute(OSL_GPE_HANDLER, inner_cb, NULL), AE_OK);
	wait_flag(&g_inner_done, 1, 5000);
	AcpiOsWaitEventsComplete();
}

/* ==========================================================================
 * Time
 * ========================================================================== */

static void t_time(void)
{
	uint64_t t0 = h_now_ns();
	UINT64 a = AcpiOsGetTimer(), b;

	AcpiOsSleep(20);
	b = AcpiOsGetTimer();
	T_ASSERT(ms_since(t0) >= 19);
	/* 100 ns units */
	T_ASSERT(b - a >= 190000ull && b - a < 100000000ull);
	t0 = h_now_ns();
	AcpiOsStall(2500);
	T_ASSERT(h_now_ns() - t0 >= 2500000ull);
	/* with interrupts off, Sleep busy-waits (no might_sleep report) */
	{
		uint64_t f = local_irq_save();

		t0 = h_now_ns();
		AcpiOsSleep(5);
		T_ASSERT(ms_since(t0) >= 4);
		local_irq_restore(f);
	}
}

int main(void);
int main(void)
{
	kshim_init_main();
	workqueue_init();
	test_case("mutex", t_mutex);
	test_case("mutex_irqs_off", t_mutex_irqs_off);
	test_case("before_scheduler", t_before_scheduler);
	test_case("semaphore", t_semaphore);
	test_case("global_lock_poll", t_global_lock_poll);
	test_case("lock_and_ids", t_lock_and_ids);
	test_case("execute_boot", t_execute_boot);
	test_case("execute_live", t_execute_live);
	test_case("wait_from_worker", t_wait_from_worker);
	test_case("time", t_time);
	test_summary_and_exit();
}
