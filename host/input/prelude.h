/* Host test prelude for the input core harness (host/test-input.sh).
 *
 * Force-included (-include) into every kernel-side unit of the harness: the
 * real kernel/dev/input/input.c, input-mt.c and evdev.c, the kernel
 * services they call (kstubs.c) and the test scenarios (tests.c).
 *
 * The kernel's own headers are used as they are -- struct input_dev, task_t
 * and the uapi structures keep their real layout -- except for the inline
 * functions that execute privileged instructions (cli/popfq in the
 * interrupt-flag helpers) or that are written for real processors
 * (spin_lock).  Those are renamed out of the way while the headers are read
 * and replaced below with host versions.
 *
 * Unlike the single-threaded filesystem harness, this one runs REAL threads
 * (producers in "interrupt context", blocking readers, register/unregister
 * loops), so the spinlocks really spin.  Every thread has its own simulated
 * interrupt flag, and every misuse is a fatal test failure:
 *
 *   - spin_lock of a lock the same thread already holds (self-deadlock);
 *   - a spin that does not get the lock within seconds (deadlock between
 *     threads: lock order inversion or a lost unlock);
 *   - spin_unlock of a lock that is not held, or held by another thread;
 *   - sleeping (mutex_lock, sched_schedule, might_sleep) with a spinlock
 *     held or "interrupts" disabled.
 *
 * BUG()/BUG_ON() print the kernel message and abort instead of executing
 * ud2.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_INPUT_PRELUDE_H
#define HOST_INPUT_PRELUDE_H

/* ---- read the kernel headers with the privileged inlines renamed ---- */
#define spinlock_init __kh_spinlock_init
#define spin_lock __kh_spin_lock
#define spin_trylock __kh_spin_trylock
#define spin_unlock __kh_spin_unlock
#define spin_is_locked __kh_spin_is_locked
#define local_irq_save __kh_local_irq_save
#define local_irq_restore __kh_local_irq_restore
#define irqs_enabled __kh_irqs_enabled
#define irqs_disabled __kh_irqs_disabled
#define spin_lock_irqsave __kh_spin_lock_irqsave
#define spin_unlock_irqrestore __kh_spin_unlock_irqrestore
#define timer_rdtsc __kh_timer_rdtsc

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>

#undef spinlock_init
#undef spin_lock
#undef spin_trylock
#undef spin_unlock
#undef spin_is_locked
#undef local_irq_save
#undef local_irq_restore
#undef irqs_enabled
#undef irqs_disabled
#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
#undef timer_rdtsc

/* ---- harness services (kstubs.c) ---- */
void test_fatal(const char *what, const char *name)
	__attribute__((noreturn));
void test_fatalf(const char *fmt, ...)
	__attribute__((noreturn, format(printf, 1, 2)));
/* Per-thread simulated interrupt flag (0x200 while "interrupts" are on)
 * and number of spinlocks the thread holds. */
extern __thread uint64_t test_irq_flag;
extern __thread int test_spins_held;
/* Small per-thread id, never 0xFFFFFFFF (the spinlock "no owner"). */
uint32_t test_thread_id(void);
/* Called while a spin_lock waits; aborts after a few seconds. */
void test_spin_wait(spinlock_t *l, unsigned long *spins);

/* ---- host side (hostlib.c, compiled against the C library) ----
 * Plain C types only: the kernel and the C library disagree about what
 * uint64_t is, so the two worlds never share a header. */
void *h_malloc(unsigned long n);
void *h_calloc(unsigned long n, unsigned long m);
void h_free(void *p);
void *h_memset(void *p, int c, unsigned long n);
void *h_memcpy(void *d, const void *s, unsigned long n);
int h_memcmp(const void *a, const void *b, unsigned long n);
unsigned long h_strlen(const char *s);
int h_strcmp(const char *a, const char *b);
int h_strncmp(const char *a, const char *b, unsigned long n);
char *h_strstr(const char *h, const char *n);
unsigned long long h_strtoull(const char *s, char **end, int base);
int h_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int h_eprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int h_vsnprintf(char *b, unsigned long n, const char *fmt,
		__builtin_va_list ap);
int h_snprintf(char *b, unsigned long n, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void h_abort(void) __attribute__((noreturn));
void h_exit(int code) __attribute__((noreturn));
char *h_getenv(const char *name);
void *h_read_file(const char *path, unsigned long *len);
void h_yield(void);
void h_usleep(unsigned int us);
unsigned long long h_monotonic_ns(void);
void h_print_stack(void);
/* Threads: start runs fn(arg) on a new thread; join waits for it. */
int h_thread_start(void *(*fn)(void *), void *arg, unsigned long *handle);
void h_thread_join(unsigned long handle);
unsigned long h_random(void);
void h_srandom(unsigned int seed);

/* ---- spinlocks: real spinning between real threads ---- */
static inline void spinlock_init(spinlock_t *l, const char *name)
{
	l->locked = 0;
	l->owner_cpu = 0xFFFFFFFF;
	l->name = name;
}

static inline void spin_lock(spinlock_t *l)
{
	unsigned long spins = 0;
	uint32_t me = test_thread_id();

	if (__atomic_load_n(&l->locked, __ATOMIC_RELAXED) &&
	    __atomic_load_n(&l->owner_cpu, __ATOMIC_RELAXED) == me)
		test_fatal("spin_lock of a lock this thread holds (deadlock)",
			   l->name);
	while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
		test_spin_wait(l, &spins);
	__atomic_store_n(&l->owner_cpu, me, __ATOMIC_RELAXED);
	test_spins_held++;
}

static inline int spin_trylock(spinlock_t *l)
{
	if (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
		return 0;
	__atomic_store_n(&l->owner_cpu, test_thread_id(), __ATOMIC_RELAXED);
	test_spins_held++;
	return 1;
}

static inline void spin_unlock(spinlock_t *l)
{
	if (!__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
		test_fatal("spin_unlock of a lock that is not held", l->name);
	if (__atomic_load_n(&l->owner_cpu, __ATOMIC_RELAXED) !=
	    test_thread_id())
		test_fatal("spin_unlock of a lock another thread holds",
			   l->name);
	__atomic_store_n(&l->owner_cpu, 0xFFFFFFFF, __ATOMIC_RELAXED);
	__atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
	test_spins_held--;
}

static inline int spin_is_locked(spinlock_t *l)
{
	return __atomic_load_n(&l->locked, __ATOMIC_RELAXED) != 0;
}

static inline uint64_t local_irq_save(void)
{
	uint64_t f = test_irq_flag;

	test_irq_flag = 0;
	return f;
}

static inline void local_irq_restore(uint64_t f)
{
	test_irq_flag = f & 0x200;
}

static inline int irqs_enabled(void)
{
	return test_irq_flag != 0;
}

static inline int irqs_disabled(void)
{
	return test_irq_flag == 0;
}

static inline void spin_lock_irqsave(spinlock_t *l, uint64_t *f)
{
	*f = local_irq_save();
	spin_lock(l);
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f)
{
	spin_unlock(l);
	local_irq_restore(f);
}

static inline uint64_t timer_rdtsc(void)
{
	return (uint64_t)h_monotonic_ns();
}

/* ---- BUG() without ud2: message, host stack trace, abort ---- */
void test_bug(const char *cond, const char *file, int line, const char *func)
	__attribute__((noreturn));
#undef BUG
#undef BUG_ON
#define BUG() test_bug(NULL, __FILE__, __LINE__, __func__)
#define BUG_ON(cond)                                                      \
	do {                                                              \
		if (unlikely(cond))                                       \
			test_bug(#cond, __FILE__, __LINE__, __func__);    \
	} while (0)

void bug_report_atomic_sleep(const char *file, int line, const char *func);
/* ---- might_sleep() always armed: interrupts off or a spinlock held ---- */
#undef might_sleep
#define might_sleep()                                                     \
	do {                                                              \
		if (unlikely(irqs_disabled() || test_spins_held))         \
			bug_report_atomic_sleep(__FILE__, __LINE__,       \
						__func__);                \
	} while (0)

#endif
