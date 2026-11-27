/* Kernel services for the input core harness (host/test-input.sh).
 *
 * Everything input.c, input-mt.c and evdev.c call that is not compiled from
 * the kernel tree itself: heap allocation on the host heap (AddressSanitizer
 * watches it), the formatter, a settable clock, one simulated task per host
 * thread with the kernel's BLOCKED/READY wait-channel protocol (a sleeping
 * thread really waits until another thread wakes its channel), sleeping
 * mutexes, the poll-queue wake hook and a recorder for device events.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/waitq.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/signal.h>
#include <kernel/fs/devevent.h>
#include <kernel/ke/syscall.h> /* errno values */
#include "kstubs.h"

/* ------------------------------------------------------------------ */
/* failure reporting                                                    */
/* ------------------------------------------------------------------ */

__thread uint64_t test_irq_flag = 0x200;
__thread int test_spins_held;
static long g_warnings;

void test_fatal(const char *what, const char *name)
{
	h_eprintf("\nFATAL: %s (%s)\n", what, name ? name : "?");
	h_print_stack();
	h_printf("FAIL fatal %s (%s)\n", what, name ? name : "?");
	h_abort();
}

void test_fatalf(const char *fmt, ...)
{
	char buf[512];
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	h_vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	test_fatal(buf, NULL);
}

void test_bug(const char *cond, const char *file, int line, const char *func)
{
	h_eprintf("BUG: %s at %s:%d %s()\n", cond ? cond : "BUG()", file, line,
		  func);
	test_fatal("kernel BUG", cond);
}

void bug_report_atomic_sleep(const char *file, int line, const char *func)
{
	h_eprintf("sleeping call with %s at %s:%d %s()\n",
		  test_spins_held ? "a spinlock held" : "interrupts off", file,
		  line, func);
	test_fatal("sleep in atomic context", func);
}

void panic(const char *fmt, ...)
{
	char buf[256];
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	h_vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	test_fatal("panic", buf);
}

long test_warning_count(void)
{
	return __atomic_load_n(&g_warnings, __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------------ */
/* threads and spinning                                                 */
/* ------------------------------------------------------------------ */

static uint32_t g_next_thread_id = 1;
static __thread uint32_t tls_thread_id;

uint32_t test_thread_id(void)
{
	if (!tls_thread_id)
		tls_thread_id = __atomic_fetch_add(&g_next_thread_id, 1,
						   __ATOMIC_RELAXED);
	return tls_thread_id;
}

void test_spin_wait(spinlock_t *l, unsigned long *spins)
{
	static const unsigned long long limit_ns = 10ULL * 1000000000ULL;
	static __thread unsigned long long since;

	if (*spins == 0)
		since = h_monotonic_ns();
	if (++*spins % 64 == 0) {
		h_yield();
		if (h_monotonic_ns() - since > limit_ns)
			test_fatal("spinlock not acquired within 10 s "
				   "(deadlock)",
				   l->name);
	}
}

/* ------------------------------------------------------------------ */
/* memory and strings                                                   */
/* ------------------------------------------------------------------ */

void *kalloc(size_t size)
{
	might_sleep();
	return h_malloc(size ? size : 1);
}

void *kcalloc(size_t count, size_t size)
{
	might_sleep();
	if (size && count > ~(size_t)0 / size)
		return NULL;
	return h_calloc(count ? count : 1, size ? size : 1);
}

void kfree(void *ptr)
{
	h_free(ptr);
}

void *kmemcpy(void *dest, const void *src, size_t size)
{
	return h_memcpy(dest, src, size);
}

void *kmemset(void *ptr, int value, size_t size)
{
	return h_memset(ptr, value, size);
}

size_t kstrlen(const char *str)
{
	return h_strlen(str);
}

void smap_disable(void)
{
}

void smap_enable(void)
{
}

int kvsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
	return h_vsnprintf(buffer, size, format, args);
}

int ksnprintf(char *buffer, size_t size, const char *format, ...)
{
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, format);
	n = h_vsnprintf(buffer, size, format, ap);
	__builtin_va_end(ap);
	return n;
}

int kprintf(const char *format, ...)
{
	char buf[512];
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, format);
	n = h_vsnprintf(buf, sizeof(buf), format, ap);
	__builtin_va_end(ap);
	if (h_strncmp(buf, "WARNING", 7) == 0) {
		__atomic_fetch_add(&g_warnings, 1, __ATOMIC_SEQ_CST);
		h_eprintf("[kernel] %s", buf);
		h_print_stack();
	} else if (h_getenv("TESTINPUT_VERBOSE")) {
		h_eprintf("[kernel] %s", buf);
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* clock                                                                */
/* ------------------------------------------------------------------ */

static uint64_t g_clock_ns;

void test_set_clock_ns(uint64_t ns)
{
	__atomic_store_n(&g_clock_ns, ns, __ATOMIC_SEQ_CST);
}

uint64_t timer_get_precise_ns(void)
{
	uint64_t ns = __atomic_load_n(&g_clock_ns, __ATOMIC_SEQ_CST);

	return ns ? ns : (uint64_t)h_monotonic_ns();
}

uint64_t timer_realtime_offset_ns(uint64_t mono)
{
	(void)mono;
	return TEST_REALTIME_OFFSET_NS;
}

/* ------------------------------------------------------------------ */
/* tasks: one per host thread, the wait-channel protocol                */
/* ------------------------------------------------------------------ */

#define TEST_MAX_TASKS 256
static task_t *g_tasks[TEST_MAX_TASKS];
static volatile int g_tasks_lock; /* a raw lock: below the spinlock checks */
static __thread task_t *tls_task;
static __thread int tls_signal;
static int g_sleepers;

static void tasks_lock(void)
{
	while (__atomic_exchange_n(&g_tasks_lock, 1, __ATOMIC_ACQUIRE))
		h_yield();
}

static void tasks_unlock(void)
{
	__atomic_store_n(&g_tasks_lock, 0, __ATOMIC_RELEASE);
}

task_t *sched_current(void)
{
	if (!tls_task) {
		int i;

		tls_task = (task_t *)h_calloc(1, sizeof(task_t));
		if (!tls_task)
			test_fatal("out of memory", "task");
		tls_task->id = test_thread_id();
		tls_task->state = TASK_RUNNING;
		tasks_lock();
		for (i = 0; i < TEST_MAX_TASKS; i++)
			if (!g_tasks[i]) {
				g_tasks[i] = tls_task;
				break;
			}
		tasks_unlock();
		if (i == TEST_MAX_TASKS)
			test_fatal("too many threads", "task");
	}
	return tls_task;
}

void test_thread_done(void)
{
	if (!tls_task)
		return;
	tasks_lock();
	for (int i = 0; i < TEST_MAX_TASKS; i++)
		if (g_tasks[i] == tls_task)
			g_tasks[i] = NULL;
	tasks_unlock();
	h_free(tls_task);
	tls_task = NULL;
}

void sched_wake_channel(void *channel)
{
	if (!channel)
		return;
	tasks_lock();
	for (int i = 0; i < TEST_MAX_TASKS; i++) {
		task_t *t = g_tasks[i];
		task_state_t from = TASK_BLOCKED;

		if (!t || __atomic_load_n(&t->wait_channel, __ATOMIC_ACQUIRE) !=
				  channel)
			continue;
		if (__atomic_compare_exchange_n(&t->state, &from, TASK_READY,
						false, __ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			__atomic_store_n(&t->wait_channel, NULL,
					 __ATOMIC_RELEASE);
	}
	tasks_unlock();
}

void sched_schedule(void)
{
	task_t *cur = sched_current();
	unsigned long long since = h_monotonic_ns();

	if (test_spins_held || !test_irq_flag)
		test_fatal("sched_schedule in atomic context", NULL);
	__atomic_fetch_add(&g_sleepers, 1, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&cur->state, __ATOMIC_ACQUIRE) ==
	       TASK_BLOCKED) {
		if (tls_signal) {
			task_state_t from = TASK_BLOCKED;

			/* A signal ends the sleep, as the real scheduler's
			 * signal wake would. */
			if (__atomic_compare_exchange_n(
				    &cur->state, &from, TASK_READY, false,
				    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
				cur->wait_channel = NULL;
			break;
		}
		h_usleep(50);
		if (h_monotonic_ns() - since > 20ULL * 1000000000ULL)
			test_fatal("a sleep nobody ended within 20 s "
				   "(lost wake-up)",
				   NULL);
	}
	/* A real task that was woken does not run before its waker is done
	 * with it (the claim, the clearing of the channel, the enqueue all
	 * happen first).  Here the waker's critical section is the task
	 * registry lock: wait for it, or a late clearing of wait_channel by
	 * the waker would hit this task's NEXT sleep. */
	tasks_lock();
	tasks_unlock();
	__atomic_fetch_sub(&g_sleepers, 1, __ATOMIC_SEQ_CST);
	cur->state = TASK_RUNNING;
}

int test_sleepers(void)
{
	return __atomic_load_n(&g_sleepers, __ATOMIC_SEQ_CST);
}

void test_set_signal_pending(int on)
{
	tls_signal = on;
}

int signal_pending(struct task *task)
{
	(void)task;
	return tls_signal;
}

/* ------------------------------------------------------------------ */
/* mutexes: sleeping locks between real threads                         */
/* ------------------------------------------------------------------ */

void mutex_init(struct mutex *m, const char *name)
{
	m->locked = 0;
	m->owner = MUTEX_NO_OWNER;
	m->name = name;
	m->contended = 0;
}

void mutex_lock(struct mutex *m)
{
	unsigned long long since = h_monotonic_ns();
	uint64_t me = test_thread_id();

	might_sleep();
	if (__atomic_load_n(&m->locked, __ATOMIC_ACQUIRE) &&
	    __atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me)
		test_fatal("mutex_lock of a mutex this thread holds", m->name);
	while (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE)) {
		h_usleep(20);
		if (h_monotonic_ns() - since > 20ULL * 1000000000ULL)
			test_fatal("mutex not acquired within 20 s", m->name);
	}
	__atomic_store_n(&m->owner, me, __ATOMIC_RELAXED);
}

int mutex_lock_interruptible(struct mutex *m)
{
	if (tls_signal)
		return -EINTR;
	mutex_lock(m);
	return 0;
}

int mutex_trylock(struct mutex *m)
{
	if (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE))
		return 0;
	__atomic_store_n(&m->owner, (uint64_t)test_thread_id(),
			 __ATOMIC_RELAXED);
	return 1;
}

void mutex_unlock(struct mutex *m)
{
	if (!__atomic_load_n(&m->locked, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&m->owner, __ATOMIC_RELAXED) != test_thread_id())
		test_fatal("mutex_unlock by a thread that does not hold it",
			   m->name);
	__atomic_store_n(&m->owner, MUTEX_NO_OWNER, __ATOMIC_RELAXED);
	__atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
}

int mutex_held(const struct mutex *m)
{
	return __atomic_load_n(&m->locked, __ATOMIC_ACQUIRE) &&
	       __atomic_load_n(&m->owner, __ATOMIC_RELAXED) ==
		       test_thread_id();
}

/* ------------------------------------------------------------------ */
/* poll queues and device events                                        */
/* ------------------------------------------------------------------ */

static unsigned long g_poll_wakes[16];

void poll_notify_wq_key(struct wait_queue_head *h, unsigned key)
{
	if (!h)
		test_fatal("poll wake of a NULL queue", NULL);
	for (unsigned b = 0; b < 16; b++)
		if (key & (1u << b))
			__atomic_fetch_add(&g_poll_wakes[b], 1,
					   __ATOMIC_RELAXED);
}

unsigned long test_poll_wakes(unsigned key_bit)
{
	for (unsigned b = 0; b < 16; b++)
		if (key_bit == (1u << b))
			return __atomic_load_n(&g_poll_wakes[b],
					       __ATOMIC_RELAXED);
	return 0;
}

#define DEVEV_RING 64
static struct test_devevent g_devev[DEVEV_RING];
static unsigned g_devev_count;
static volatile int g_devev_lock;

void devevent_publish(uint32_t type, uint32_t device_class,
		      uint64_t device_id, uint64_t parent_id, uint32_t flags,
		      uint16_t vendor, uint16_t product, uint32_t major,
		      uint32_t minor, const char *name)
{
	struct test_devevent *e;

	(void)parent_id;
	while (__atomic_exchange_n(&g_devev_lock, 1, __ATOMIC_ACQUIRE))
		h_yield();
	e = &g_devev[g_devev_count % DEVEV_RING];
	h_memset(e, 0, sizeof(*e));
	e->type = type;
	e->device_class = device_class;
	e->device_id = device_id;
	e->flags = flags;
	e->vendor = vendor;
	e->product = product;
	e->major = major;
	e->minor = minor;
	if (name)
		h_snprintf(e->name, sizeof(e->name), "%s", name);
	g_devev_count++;
	__atomic_store_n(&g_devev_lock, 0, __ATOMIC_RELEASE);
}

unsigned test_devevent_count(void)
{
	return __atomic_load_n(&g_devev_count, __ATOMIC_ACQUIRE);
}

int test_devevent_get(unsigned index, struct test_devevent *out)
{
	int ok = 0;

	while (__atomic_exchange_n(&g_devev_lock, 1, __ATOMIC_ACQUIRE))
		h_yield();
	if (index < g_devev_count && g_devev_count - index <= DEVEV_RING) {
		*out = g_devev[index % DEVEV_RING];
		ok = 1;
	}
	__atomic_store_n(&g_devev_lock, 0, __ATOMIC_RELEASE);
	return ok;
}
