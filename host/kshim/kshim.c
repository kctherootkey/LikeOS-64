/* Kernel side of the kernel shim (host/kshim): the kernel services the
 * code under test calls -- console, memory, tasks, mutexes, high-resolution
 * timers, delays, the bits of the ACPI table code acpi_dev.c uses -- built
 * on the host services of hrt.c, plus the test-case runner.  Compiled
 * freestanding with host/kshim/prelude.h.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/delay.h>

__thread uint64_t t_irqflag;
__thread int t_spins_held;
volatile int g_test_warnings;
static __thread task_t *t_current;

/* ---- console ---------------------------------------------------------- */

int kprintf(const char *fmt, ...)
{
	char buf[1024];
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = h_vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	h_printf("%s", buf);
	return n;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, fmt);
	n = h_vsnprintf(buf, size, fmt, ap);
	__builtin_va_end(ap);
	return n;
}

size_t kstrlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

int kstrcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int kstrncmp(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i] || !a[i])
			return (unsigned char)a[i] - (unsigned char)b[i];
	}
	return 0;
}

void *kmemset(void *p, int v, size_t n)
{
	unsigned char *d = p;

	for (size_t i = 0; i < n; i++)
		d[i] = (unsigned char)v;
	return p;
}

void *kmemcpy(void *d, const void *s, size_t n)
{
	unsigned char *dd = d;
	const unsigned char *ss = s;

	for (size_t i = 0; i < n; i++)
		dd[i] = ss[i];
	return d;
}

int kmemcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (size_t i = 0; i < n; i++)
		if (x[i] != y[i])
			return x[i] - y[i];
	return 0;
}


/* ---- memory ----------------------------------------------------------- */

void *kalloc(size_t size)
{
	return h_calloc(1, size);
}

void *kcalloc(size_t count, size_t size)
{
	if (size && count > ~(size_t)0 / size)
		return NULL;
	return h_calloc(count, size);
}

void kfree(void *p)
{
	h_free(p);
}

uint64_t mm_map_device_mmio(uint64_t phys, size_t pages)
{
	/* The tests hand "physical" addresses that are host pointers. */
	(void)pages;
	return phys;
}

void *mm_alloc_guarded_kstack(size_t size)
{
	return h_calloc(1, size);
}

void mm_free_guarded_kstack(void *stack, size_t size)
{
	(void)size;
	h_free(stack);
}

/* ---- assertions ---------------------------------------------------------- */

void test_bug(const char *cond, const char *file, int line, const char *func)
{
	h_printf("BUG: %s at %s:%d %s()\n", cond ? cond : "BUG()", file, line,
		 func);
	h_print_stack();
	h_abort();
}

void test_warn(const char *cond, const char *file, int line)
{
	__atomic_add_fetch(&g_test_warnings, 1, __ATOMIC_RELAXED);
	h_printf("WARNING: %s at %s:%d\n", cond, file, line);
}

void bug_report_atomic_sleep(const char *file, int line, const char *func)
{
	h_printf("BUG: sleeping function called from atomic context (irqs %s, %d spinlocks) at %s:%d %s()\n",
		 irqs_disabled() ? "off" : "on", t_spins_held, file, line,
		 func);
	h_print_stack();
	h_abort();
}

/* ---- tasks ------------------------------------------------------------- */

#define KSHIM_MAX_TASKS 4096
static task_t *g_tasks[KSHIM_MAX_TASKS];
static volatile uint64_t g_next_task_id = 1;
static spinlock_t g_task_lock = SPINLOCK_INIT("kshim_tasks");

struct task *kshim_current_or_null(void)
{
	return t_current;
}

task_t *sched_current(void)
{
	return t_current;
}

static task_t *task_new(task_entry_t entry, void *arg)
{
	task_t *t = h_calloc(1, sizeof(*t));
	uint64_t fl;

	if (!t)
		return NULL;
	t->id = __atomic_fetch_add(&g_next_task_id, 1, __ATOMIC_RELAXED);
	t->entry = entry;
	t->arg = arg;
	spin_lock_irqsave(&g_task_lock, &fl);
	if (t->id < KSHIM_MAX_TASKS)
		g_tasks[t->id] = t;
	spin_unlock_irqrestore(&g_task_lock, fl);
	return t;
}

void kshim_init_main(void)
{
	t_irqflag = 0x200;
	t_spins_held = 0;
	if (!t_current)
		t_current = task_new(NULL, NULL);
	ksnprintf(t_current->comm, sizeof(t_current->comm), "main");
}

static void task_trampoline(void *p)
{
	task_t *t = p;

	t_current = t;
	t_irqflag = 0x200;
	t_spins_held = 0;
	t->entry(t->arg);
	if (t_spins_held || irqs_disabled())
		test_bug("task returned with a spinlock or irqs off", __FILE__,
			 __LINE__, t->comm);
	__atomic_store_n(&t->gone, 1, __ATOMIC_RELEASE);
	h_wq_wake();
}

task_t *sched_add_task(task_entry_t entry, void *arg, void *stack,
		       size_t size)
{
	task_t *t;

	(void)stack;
	(void)size;
	t = task_new(entry, arg);
	if (!t)
		return NULL;
	if (h_thread_create(task_trampoline, t) != 0)
		return NULL;
	return t;
}

task_t *sched_find_task_by_id(uint32_t pid)
{
	task_t *t = NULL;
	uint64_t fl;

	spin_lock_irqsave(&g_task_lock, &fl);
	if (pid < KSHIM_MAX_TASKS)
		t = g_tasks[pid];
	spin_unlock_irqrestore(&g_task_lock, fl);
	if (t && __atomic_load_n(&t->gone, __ATOMIC_ACQUIRE))
		return NULL;
	return t;
}

void sched_set_user_nice(task_t *t, int nice)
{
	(void)t;
	(void)nice;
}

void sched_yield_in_kernel(void)
{
	h_yield();
}

/* ---- mutexes ------------------------------------------------------------ */

void mutex_init(struct mutex *m, const char *name)
{
	m->locked = 0;
	m->owner = NULL;
	m->name = name;
}

void mutex_lock(struct mutex *m)
{
	might_sleep();
	if (m->locked && m->owner == (void *)sched_current())
		test_bug("mutex_lock recursion", __FILE__, __LINE__, m->name);
	for (;;) {
		unsigned long seq = h_wq_seq();

		if (!__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE))
			break;
		h_wq_wait(seq);
	}
	m->owner = sched_current();
}

int mutex_trylock(struct mutex *m)
{
	if (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE))
		return 0;
	m->owner = sched_current();
	return 1;
}

void mutex_unlock(struct mutex *m)
{
	if (!m->locked || m->owner != (void *)sched_current())
		test_bug("mutex_unlock by a non-owner", __FILE__, __LINE__,
			 m->name);
	m->owner = NULL;
	__atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
	h_wq_wake();
}

/* ---- high-resolution timers: one timer thread --------------------------- */

static spinlock_t g_hrt_lock = SPINLOCK_INIT("kshim_hrtimer");
static hrtimer_t *g_hrt_head;
static volatile int g_hrt_thread;
/* The timer whose callback is between its unlink and its end, and how
 * often a cancel met a timer in that window (coverage of the callers'
 * in-flight handling). */
static hrtimer_t *volatile g_hrt_firing;
volatile unsigned long g_hrt_inflight_cancels;

uint64_t hrtimer_now_ns(void)
{
	return h_now_ns();
}

void hrtimer_init(hrtimer_t *t, hrtimer_fn_t fn, void *arg)
{
	t->fn = fn;
	t->arg = arg;
	t->next = NULL;
	t->armed = 0;
	t->cpu = 0;
	t->expires_ns = 0;
}

static void hrt_unlink_locked(hrtimer_t *t)
{
	hrtimer_t **pp = &g_hrt_head;

	while (*pp && *pp != t)
		pp = &(*pp)->next;
	if (*pp)
		*pp = t->next;
	t->next = NULL;
}

static void hrt_thread(void *arg)
{
	(void)arg;
	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&g_hrt_lock, &fl);
		while (g_hrt_head && g_hrt_head->expires_ns <= h_now_ns()) {
			hrtimer_t *t = g_hrt_head;

			g_hrt_head = t->next;
			t->next = NULL;
			__atomic_store_n(&t->armed, 0, __ATOMIC_RELEASE);
			g_hrt_firing = t;
			/* The callback runs with "interrupts" off and the
			 * queue unlocked, as in the kernel -- a little late
			 * now and then, to widen the window in which a
			 * cancel finds the timer fired but its callback not
			 * through yet. */
			spin_unlock(&g_hrt_lock);
			if ((h_rand() & 3) == 0)
				udelay(h_rand() % 40);
			t->fn(t);
			g_hrt_firing = NULL;
			spin_lock(&g_hrt_lock);
		}
		spin_unlock_irqrestore(&g_hrt_lock, fl);
		h_usleep(20);
	}
}

void hrtimer_start(hrtimer_t *t, uint64_t abs_ns)
{
	hrtimer_t **pp;
	uint64_t fl;

	if (!__atomic_exchange_n(&g_hrt_thread, 1, __ATOMIC_ACQ_REL))
		(void)sched_add_task(hrt_thread, NULL, NULL, 0);
	spin_lock_irqsave(&g_hrt_lock, &fl);
	if (t->armed)
		hrt_unlink_locked(t);
	t->expires_ns = abs_ns;
	pp = &g_hrt_head;
	while (*pp && (*pp)->expires_ns <= abs_ns)
		pp = &(*pp)->next;
	t->next = *pp;
	*pp = t;
	__atomic_store_n(&t->armed, 1, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&g_hrt_lock, fl);
}

int hrtimer_cancel(hrtimer_t *t)
{
	uint64_t fl;
	int was;

	if (g_hrt_firing == t)
		__atomic_add_fetch(&g_hrt_inflight_cancels, 1,
				   __ATOMIC_RELAXED);
	if (!__atomic_load_n(&t->armed, __ATOMIC_ACQUIRE))
		return 0;
	spin_lock_irqsave(&g_hrt_lock, &fl);
	was = t->armed;
	if (was)
		hrt_unlink_locked(t);
	t->armed = 0;
	spin_unlock_irqrestore(&g_hrt_lock, fl);
	return was;
}

int hrtimer_sleep_until(uint64_t abs_ns, uint64_t *remaining_ns)
{
	uint64_t now = h_now_ns();

	if (remaining_ns)
		*remaining_ns = 0;
	if (abs_ns > now)
		h_usleep((abs_ns - now) / 1000 + 1);
	return 0;
}

/* ---- delays ------------------------------------------------------------ */

int delay_can_sleep(void)
{
	return sched_current() != NULL && !irqs_disabled();
}

void udelay(unsigned long usecs)
{
	uint64_t until = h_now_ns() + (uint64_t)usecs * 1000;

	while (h_now_ns() < until)
		;
}

void mdelay(unsigned long msecs)
{
	udelay(msecs * 1000);
}

void msleep(unsigned int msecs)
{
	might_sleep();
	h_usleep((unsigned long)msecs * 1000);
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
	h_usleep(min_us);
}

void fsleep(unsigned long usecs)
{
	h_usleep(usecs);
}

/* ---- ACPI tables --------------------------------------------------------- */

int g_kshim_acpi_ready = 1;
irq_override_t g_kshim_overrides[4] = {
	{ 0, 2, 0, 0 }, /* ISA 0 -> GSI 2 */
	{ 9, 9, 3, 3 }, /* SCI: level, active low */
};

int acpi_namespace_ready(void)
{
	return g_kshim_acpi_ready;
}

irq_override_t *acpi_get_irq_override(uint8_t isa_irq)
{
	for (int i = 0; i < 2; i++)
		if (g_kshim_overrides[i].bus_irq == isa_irq)
			return &g_kshim_overrides[i];
	return NULL;
}

/* ---- test runner ----------------------------------------------------------- */

static int g_cases, g_failed_warn;

void test_case(const char *name, void (*fn)(void))
{
	int w0 = g_test_warnings;
	uint64_t t0 = h_now_ns();

	h_printf("RUN  %s\n", name);
	fn();
	if (t_spins_held || irqs_disabled())
		test_bug("test case leaked a spinlock or irqs off", __FILE__,
			 __LINE__, name);
	if (g_test_warnings != w0) {
		h_printf("FAIL %s: %d unexpected warning(s)\n", name,
			 g_test_warnings - w0);
		g_failed_warn++;
		return;
	}
	g_cases++;
	h_printf("PASS %s (%llu ms)\n", name,
		 (unsigned long long)((h_now_ns() - t0) / 1000000));
}

void test_summary_and_exit(void)
{
	h_printf("%d passed, %d failed\n", g_cases, g_failed_warn);
	h_exit(g_failed_warn ? 1 : 0);
}
