/*
 * oslikeos_sync.c - ACPICA OS Services Layer for LikeOS: threads,
 * synchronisation, deferred execution and time.
 *
 * The threading model:
 *
 *   * ACPICA serialises AML itself (its interpreter, namespace and cache
 *     mutexes, the AML Mutex objects, the operation region context
 *     mutexes), so these primitives must really exclude:
 *       - mutexes and semaphores sleep (with the timeouts ACPICA asks
 *         for: 0 = try once, ACPI_WAIT_FOREVER, else milliseconds); a
 *         mutex is a binary semaphore -- ACPICA's namespace reader lock
 *         takes its writer mutex in the first reader and releases it in
 *         the last one, which may be another thread;
 *       - spinlocks disable interrupts (ACPICA takes its GPE and hardware
 *         locks from the SCI handler as well) and hand the saved flags
 *         back;
 *       - AcpiOsGetThreadId is the calling task: the owner checks of the
 *         AML Mutex objects and of the global lock depend on it.
 *   * Interrupt context (the SCI handler: fixed events, GPE detection and
 *     the EC's GPE handler) never runs AML.  ACPICA hands every method it
 *     wants run there -- _Lxx/_Exx, Notify handlers -- to AcpiOsExecute,
 *     which queues it on an ordered work queue: kacpid for GPE methods
 *     (and the other non-notify kinds), kacpi_notify for Notify().  The EC
 *     driver (kernel/hal/acpi.c) does the same with its queries.
 *   * Before the work queues exist (acpi_init runs on the boot CPU before
 *     the scheduler, with interrupts off) the deferred calls are kept in
 *     a list, in order: the boot-time event poll runs them right after the
 *     SCI handler returned (acpi_os_run_boot_deferred), and whatever is
 *     left goes onto the queues when they are created
 *     (acpi_os_initialize1).  Never inside the SCI handler: ACPICA holds
 *     its GPE spinlock around the dispatch.
 *   * A context that cannot sleep -- before the scheduler, with
 *     interrupts off -- waits by polling: before the scheduler there is
 *     one thread and nothing that could signal, so a wait for longer than
 *     its time can never succeed and fails at once (a finite timeout is
 *     still waited out: AML uses Wait() as a delay); with interrupts off
 *     and other CPUs running, the unit is polled for, at most 10 s for
 *     "forever".
 *   * The global lock: ACPICA waits on its semaphore for the firmware's
 *     release interrupt (GBL_RLS through the SCI).  The SCI is masked on
 *     this kernel once the scheduler runs (kernel/ke/init.c), so that
 *     wait is a short poll after which ACPICA tries the hardware lock
 *     again.
 *
 * Portions Copyright (C) 2000 Andrew Henroid
 * Portions Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 * Portions Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 * Portions Copyright (c) 2008 Intel Corporation
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "acpi.h"
#include "accommon.h"

#include <kernel/ke/sched.h>
#include <kernel/ke/completion.h>
#include <kernel/ke/workqueue.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* kernel/hal/acpica/oslikeos.c: wait until no SCI handler runs. */
void acpi_osl_sci_sync(void);

#define OSL_SEM_MAGIC 0x41534d31u /* "ASM1" */
/* "Forever" with interrupts off: give up after this long. */
#define OSL_ATOMIC_FOREVER_US 10000000ull
#define OSL_ATOMIC_POLL_US 10u
/* One poll of the global lock's hardware bit. */
#define OSL_GLK_POLL_MS 1

/* A semaphore (and, with one unit, a mutex): the units are a
 * completion's events -- a counting semaphore with timed and untimed
 * waits that may be signalled from interrupt context. */
struct acpi_os_sem {
	struct completion units;
	uint32_t magic;
	uint32_t is_mutex;
};

/* ==========================================================================
 * Contexts
 * ========================================================================== */

/* May the caller sleep: a task, interrupts on. */
static int osl_can_sleep(void)
{
	return delay_can_sleep();
}

ACPI_THREAD_ID
AcpiOsGetThreadId(void)
{
	task_t *t = sched_current();

	/* Before the scheduler there is one thread, the boot CPU's. */
	if (!t)
		return (ACPI_THREAD_ID)1;
	return (ACPI_THREAD_ID)(unsigned long)t;
}

/* ==========================================================================
 * Semaphores and mutexes
 * ========================================================================== */

static struct acpi_os_sem *sem_new(UINT32 initial, int is_mutex)
{
	struct acpi_os_sem *s = kcalloc(1, sizeof(*s));

	if (!s)
		return NULL;
	init_completion(&s->units);
	s->units.done = initial;
	s->magic = OSL_SEM_MAGIC;
	s->is_mutex = (uint32_t)is_mutex;
	return s;
}

static struct acpi_os_sem *sem_of(void *handle)
{
	struct acpi_os_sem *s = handle;

	if (!s || WARN_ON_ONCE(s->magic != OSL_SEM_MAGIC))
		return NULL;
	return s;
}

static void sem_free(struct acpi_os_sem *s)
{
	s->magic = 0;
	kfree(s);
}

/* Take one unit where sleeping is not possible. */
static ACPI_STATUS sem_down_atomic(struct acpi_os_sem *s, UINT16 timeout)
{
	uint64_t limit, waited = 0;

	if (!sched_current()) {
		/* One thread, interrupts off: nobody can signal.  A finite
		 * wait still takes its time (AML Wait() as a delay). */
		if (timeout == ACPI_WAIT_FOREVER) {
			WARN_RATELIMIT(1, "ACPI: endless wait before the scheduler");
			return AE_TIME;
		}
		mdelay(timeout);
		return try_wait_for_completion(&s->units) ? AE_OK : AE_TIME;
	}
	limit = timeout == ACPI_WAIT_FOREVER ? OSL_ATOMIC_FOREVER_US :
					       (uint64_t)timeout * 1000ull;
	for (;;) {
		if (try_wait_for_completion(&s->units))
			return AE_OK;
		if (waited >= limit)
			break;
		udelay(OSL_ATOMIC_POLL_US);
		waited += OSL_ATOMIC_POLL_US;
	}
	WARN_RATELIMIT(timeout == ACPI_WAIT_FOREVER,
		       "ACPI: %s still taken after %llu ms with interrupts off",
		       s->is_mutex ? "mutex" : "semaphore",
		       (unsigned long long)(limit / 1000ull));
	return AE_TIME;
}

static ACPI_STATUS sem_down(struct acpi_os_sem *s, UINT16 timeout)
{
	if (try_wait_for_completion(&s->units))
		return AE_OK;
	if (timeout == ACPI_DO_NOT_WAIT)
		return AE_TIME;
	/* The global lock's release interrupt does not come (see the top
	 * of the file): wait a little, then let ACPICA retry the lock. */
	if ((void *)s == (void *)AcpiGbl_GlobalLockSemaphore &&
	    timeout == ACPI_WAIT_FOREVER) {
		if (osl_can_sleep())
			(void)wait_for_completion_timeout(&s->units,
							  OSL_GLK_POLL_MS);
		else
			udelay(100);
		return AE_OK;
	}
	if (!osl_can_sleep())
		return sem_down_atomic(s, timeout);
	if (timeout == ACPI_WAIT_FOREVER) {
		wait_for_completion(&s->units);
		return AE_OK;
	}
	return wait_for_completion_timeout(&s->units, (long)timeout) ? AE_OK :
								      AE_TIME;
}

ACPI_STATUS
AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits,
		      ACPI_SEMAPHORE *OutHandle)
{
	struct acpi_os_sem *s;

	(void)MaxUnits; /* not enforced (signalled units just add up) */
	if (!OutHandle)
		return AE_BAD_PARAMETER;
	s = sem_new(InitialUnits, 0);
	if (!s)
		return AE_NO_MEMORY;
	*OutHandle = (ACPI_SEMAPHORE)s;
	return AE_OK;
}

ACPI_STATUS
AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle)
{
	struct acpi_os_sem *s = sem_of(Handle);

	if (!s)
		return AE_BAD_PARAMETER;
	sem_free(s);
	return AE_OK;
}

ACPI_STATUS
AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout)
{
	struct acpi_os_sem *s = sem_of(Handle);

	if (!s || Units < 1)
		return AE_BAD_PARAMETER;
	if (Units > 1)
		return AE_SUPPORT;
	return sem_down(s, Timeout);
}

ACPI_STATUS
AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units)
{
	struct acpi_os_sem *s = sem_of(Handle);

	if (!s || Units < 1)
		return AE_BAD_PARAMETER;
	if (Units > 1)
		return AE_SUPPORT;
	complete(&s->units); /* any context */
	return AE_OK;
}

ACPI_STATUS
AcpiOsCreateMutex(ACPI_MUTEX *OutHandle)
{
	struct acpi_os_sem *s;

	if (!OutHandle)
		return AE_BAD_PARAMETER;
	s = sem_new(1, 1);
	if (!s)
		return AE_NO_MEMORY;
	*OutHandle = (ACPI_MUTEX)s;
	return AE_OK;
}

void
AcpiOsDeleteMutex(ACPI_MUTEX Handle)
{
	struct acpi_os_sem *s = sem_of(Handle);

	if (s)
		sem_free(s);
}

ACPI_STATUS
AcpiOsAcquireMutex(ACPI_MUTEX Handle, UINT16 Timeout)
{
	struct acpi_os_sem *s = sem_of(Handle);

	if (!s)
		return AE_BAD_PARAMETER;
	return sem_down(s, Timeout);
}

void
AcpiOsReleaseMutex(ACPI_MUTEX Handle)
{
	struct acpi_os_sem *s = sem_of(Handle);

	if (!s)
		return;
	/* A free mutex released again would let two holders in later. */
	if (WARN_ON_ONCE(completion_done(&s->units)))
		return;
	complete(&s->units);
}

/* ==========================================================================
 * Spinlocks
 * ========================================================================== */

ACPI_STATUS
AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle)
{
	spinlock_t *l;

	if (!OutHandle)
		return AE_BAD_PARAMETER;
	l = kcalloc(1, sizeof(*l));
	if (!l)
		return AE_NO_MEMORY;
	spinlock_init(l, "acpica");
	*OutHandle = (ACPI_SPINLOCK)l;
	return AE_OK;
}

void
AcpiOsDeleteLock(ACPI_SPINLOCK Handle)
{
	kfree(Handle);
}

ACPI_CPU_FLAGS
AcpiOsAcquireLock(ACPI_SPINLOCK Handle)
{
	uint64_t flags;

	spin_lock_irqsave((spinlock_t *)Handle, &flags);
	return (ACPI_CPU_FLAGS)flags;
}

void
AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
{
	spin_unlock_irqrestore((spinlock_t *)Handle, (uint64_t)Flags);
}

/* ==========================================================================
 * Deferred execution
 * ========================================================================== */

struct acpi_os_dpc {
	struct work_struct work;
	ACPI_OSD_EXEC_CALLBACK function;
	void *context;
	int notify; /* kacpi_notify, else kacpid */
	struct acpi_os_dpc *next; /* the boot list */
};

enum { OSL_BOOT = 0, OSL_LIVE = 1, OSL_FAILED = 2 };

static spinlock_t g_osl_lock = SPINLOCK_INIT("acpi_osl");
static int g_osl_state; /* OSL_* (g_osl_lock; read with acquire) */
static struct workqueue_struct *g_kacpid_wq;
static struct workqueue_struct *g_kacpi_notify_wq;
static struct acpi_os_dpc *g_boot_head;
static struct acpi_os_dpc **g_boot_tail = &g_boot_head;

static void acpi_os_execute_deferred(struct work_struct *work)
{
	struct acpi_os_dpc *dpc = container_of(work, struct acpi_os_dpc, work);

	dpc->function(dpc->context);
	kfree(dpc); /* the worker does not touch the item after this */
}

static struct workqueue_struct *dpc_queue(const struct acpi_os_dpc *dpc)
{
	return dpc->notify ? g_kacpi_notify_wq : g_kacpid_wq;
}

ACPI_STATUS
AcpiOsExecute(ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function,
	      void *Context)
{
	struct acpi_os_dpc *dpc;
	uint64_t fl;
	int notify;

	if (!Function)
		return AE_BAD_PARAMETER;
	switch (Type) {
	case OSL_NOTIFY_HANDLER:
		notify = 1;
		break;
	case OSL_GPE_HANDLER:
	case OSL_GLOBAL_LOCK_HANDLER:
	case OSL_EC_POLL_HANDLER:
	case OSL_EC_BURST_HANDLER:
		notify = 0;
		break;
	default:
		/* The debugger threads: not built. */
		kprintf("ACPI: unsupported deferred call type %d\n", (int)Type);
		return AE_ERROR;
	}
	/* Any context, the SCI handler included (slab allocations are
	 * interrupt-safe). */
	dpc = kcalloc(1, sizeof(*dpc));
	if (!dpc)
		return AE_NO_MEMORY;
	dpc->function = Function;
	dpc->context = Context;
	dpc->notify = notify;
	INIT_WORK(&dpc->work, acpi_os_execute_deferred);

	spin_lock_irqsave(&g_osl_lock, &fl);
	if (g_osl_state == OSL_BOOT) {
		*g_boot_tail = dpc;
		g_boot_tail = &dpc->next;
		spin_unlock_irqrestore(&g_osl_lock, fl);
		return AE_OK;
	}
	if (g_osl_state == OSL_LIVE) {
		(void)queue_work(dpc_queue(dpc), &dpc->work);
		spin_unlock_irqrestore(&g_osl_lock, fl);
		return AE_OK;
	}
	spin_unlock_irqrestore(&g_osl_lock, fl);
	/* No queues (their creation failed): running AML here could mean
	 * running it in the SCI handler.  ACPICA leaves the event
	 * disabled. */
	kfree(dpc);
	return AE_ERROR;
}

void acpi_os_run_boot_deferred(void)
{
	for (;;) {
		struct acpi_os_dpc *dpc;
		uint64_t fl;

		spin_lock_irqsave(&g_osl_lock, &fl);
		dpc = g_osl_state == OSL_BOOT ? g_boot_head : NULL;
		if (dpc) {
			g_boot_head = dpc->next;
			if (!g_boot_head)
				g_boot_tail = &g_boot_head;
		}
		spin_unlock_irqrestore(&g_osl_lock, fl);
		if (!dpc)
			return;
		dpc->function(dpc->context);
		kfree(dpc);
	}
}

int acpi_os_initialize1(void)
{
	struct workqueue_struct *gpe, *notify;
	struct acpi_os_dpc *dpc, *next;
	uint64_t fl;

	gpe = alloc_ordered_workqueue("kacpid", 0);
	notify = alloc_ordered_workqueue("kacpi_notify", 0);
	spin_lock_irqsave(&g_osl_lock, &fl);
	if (!gpe || !notify) {
		g_osl_state = OSL_FAILED;
		spin_unlock_irqrestore(&g_osl_lock, fl);
		kprintf("ACPI: no work queues: firmware events stay disabled\n");
		/* Events deferred at boot run now, in this thread. */
		dpc = g_boot_head;
		g_boot_head = NULL;
		g_boot_tail = &g_boot_head;
		for (; dpc; dpc = next) {
			next = dpc->next;
			dpc->function(dpc->context);
			kfree(dpc);
		}
		if (gpe)
			destroy_workqueue(gpe);
		if (notify)
			destroy_workqueue(notify);
		return -ENOMEM;
	}
	g_kacpid_wq = gpe;
	g_kacpi_notify_wq = notify;
	/* What the boot left, in its order, ahead of anything new. */
	for (dpc = g_boot_head; dpc; dpc = next) {
		next = dpc->next;
		(void)queue_work(dpc_queue(dpc), &dpc->work);
	}
	g_boot_head = NULL;
	g_boot_tail = &g_boot_head;
	__atomic_store_n(&g_osl_state, OSL_LIVE, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&g_osl_lock, fl);
	return 0;
}

int acpi_os_deferral_live(void)
{
	return __atomic_load_n(&g_osl_state, __ATOMIC_ACQUIRE) == OSL_LIVE;
}

void
AcpiOsWaitEventsComplete(void)
{
	/* The GPE and fixed event handlers are not running on another CPU
	 * after this... */
	acpi_osl_sci_sync();
	if (__atomic_load_n(&g_osl_state, __ATOMIC_ACQUIRE) != OSL_LIVE) {
		acpi_os_run_boot_deferred();
		return;
	}
	/* ...and what they deferred has run.  Not from a worker of the
	 * queue itself (its own item would never finish). */
	if (WARN_ON_ONCE(!osl_can_sleep()))
		return;
	if (!current_is_worker_of(g_kacpid_wq))
		flush_workqueue(g_kacpid_wq);
	if (!current_is_worker_of(g_kacpi_notify_wq))
		flush_workqueue(g_kacpi_notify_wq);
}

/* ==========================================================================
 * Time
 * ========================================================================== */

void
AcpiOsStall(UINT32 Microseconds)
{
	/* In chunks: a long stall (the S5 retry waits seconds) keeps each
	 * busy-wait short. */
	while (Microseconds) {
		UINT32 d = Microseconds > 1000 ? 1000 : Microseconds;

		udelay(d);
		Microseconds -= d;
	}
}

void
AcpiOsSleep(UINT64 Milliseconds)
{
	/* Sleeps in a task with interrupts on, busy-waits otherwise. */
	while (Milliseconds) {
		unsigned int d = Milliseconds > 1000000u ? 1000000u :
							  (unsigned int)Milliseconds;

		if (osl_can_sleep())
			msleep(d);
		else
			mdelay(d);
		Milliseconds -= d;
	}
}

UINT64
AcpiOsGetTimer(void)
{
	/* 100 ns units of the monotonic clock. */
	return (UINT64)(hrtimer_now_ns() / 100ull);
}
