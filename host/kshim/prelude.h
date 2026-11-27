/* Host test prelude for the GPIO / pin controller, ACPI device layer and
 * work queue harnesses (host/test-pinctrl.sh, host/test-acpidev.sh,
 * host/test-workqueue.sh).
 *
 * Force-included (-include) into every kernel-side unit of those
 * harnesses: the kernel sources under test and the harness's kernel-side
 * shim and tests.  Unlike the filesystem harness, these programs run
 * several threads (work queue workers, timer callbacks, racing
 * queuers/cancellers), so the kernel's locking headers are replaced, not
 * adapted: the include guards of sched.h, waitq.h, mutex.h, memory.h,
 * console.h, syscall.h, bug.h, acpi.h and pci.h are taken here and the
 * few declarations the code under test uses are provided with host
 * semantics:
 *
 *   - spinlocks are real test-and-set locks (threads contend for them);
 *     the "interrupt flag" is per thread, and taking a spinlock, holding
 *     one while sleeping, or sleeping with interrupts off is checked;
 *   - wait queues are one condition variable with a wake sequence, so a
 *     wake between a waiter's test and its sleep is never lost;
 *   - tasks are pthreads; sched_current() is the thread's own task;
 *   - BUG/WARN print and count; a test fails on any WARN it did not
 *     expect.
 *
 * Everything that needs the C library is behind h_* functions (hrt.c):
 * the kernel and the C library disagree about basic types, so the two
 * worlds never share a header.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_KSHIM_PRELUDE_H
#define HOST_KSHIM_PRELUDE_H

/* Take the guards of the headers replaced here. */
#define _KERNEL_SCHED_H_
#define _KERNEL_WAITQ_H_
#define KERNEL_KE_MUTEX_H
#define MEMORY_H
#define _KERNEL_CONSOLE_H_
#define _KERNEL_SYSCALL_H_
#define _KERNEL_BUG_H_
#define _KERNEL_ACPI_H_
/* A harness that drives PCI drivers defines KSHIM_REAL_PCI and gets the
 * kernel's own pci.h (and provides the PCI functions it uses). */
#ifndef KSHIM_REAL_PCI
#define LIKEOS_PCI_H
#endif

#include <kernel/uapi/types.h>

#ifndef NULL
#define NULL ((void *)0)
#endif
#define MAX_CPUS 64

/* ---- host side (hrt.c, compiled against the C library) ---- */
int h_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int h_vsnprintf(char *b, unsigned long n, const char *fmt,
		__builtin_va_list ap);
void *h_calloc(unsigned long n, unsigned long m);
void h_free(void *p);
void h_abort(void) __attribute__((noreturn));
void h_exit(int code) __attribute__((noreturn));
void h_yield(void);
void h_usleep(unsigned long us);
unsigned long long h_now_ns(void);
int h_thread_create(void (*fn)(void *), void *arg);
/* the global wake sequence of all wait queues */
unsigned long h_wq_seq(void);
void h_wq_wait(unsigned long seq);
void h_wq_wake(void);
unsigned long h_rand(void);
void h_print_stack(void);

/* ---- errno ---- */
#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define EINVAL 22
#define ENOSPC 28
#define ERANGE 34
#define ENOSYS 38
#define ENODATA 61
#define EPROTO 71
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
#define EREMOTEIO 121
#define ERESTARTSYS 512

/* ---- console ---- */
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int ksnprintf(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
size_t kstrlen(const char *s);
int kstrcmp(const char *a, const char *b);
int kstrncmp(const char *a, const char *b, size_t n);
void *kmemset(void *p, int v, size_t n);
void *kmemcpy(void *d, const void *s, size_t n);
int kmemcmp(const void *a, const void *b, size_t n);

/* ---- memory ---- */
void *kalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void kfree(void *p);
uint64_t mm_map_device_mmio(uint64_t phys, size_t pages);
void *mm_alloc_guarded_kstack(size_t size);
void mm_free_guarded_kstack(void *stack, size_t size);

/* ---- assertions (counted) ---- */
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
extern volatile int g_test_warnings;
void test_bug(const char *cond, const char *file, int line, const char *func)
	__attribute__((noreturn));
void test_warn(const char *cond, const char *file, int line);
#define BUG() test_bug(NULL, __FILE__, __LINE__, __func__)
#define BUG_ON(c)                                                         \
	do {                                                              \
		if (unlikely(c))                                          \
			test_bug(#c, __FILE__, __LINE__, __func__);       \
	} while (0)
#define WARN_ON(c)                                                        \
	({                                                                \
		int __w = !!(c);                                          \
		if (unlikely(__w))                                        \
			test_warn(#c, __FILE__, __LINE__);                \
		__w;                                                      \
	})
#define WARN(c, fmt, ...)                                                 \
	({                                                                \
		int __w = !!(c);                                          \
		if (unlikely(__w)) {                                      \
			kprintf("WARNING: " fmt "\n", ##__VA_ARGS__);     \
			test_warn(#c, __FILE__, __LINE__);                \
		}                                                         \
		__w;                                                      \
	})
#define WARN_ON_ONCE(c) WARN_ON(c)
#define WARN_RATELIMIT(c, fmt, ...) WARN(c, fmt, ##__VA_ARGS__)
#define VM_BUG_ON(c) BUG_ON(c)
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON: " #c)

typedef struct {
	volatile int counter;
} refcount_t;
static inline void refcount_set(refcount_t *r, int n)
{
	__atomic_store_n(&r->counter, n, __ATOMIC_RELEASE);
}
static inline int refcount_read(const refcount_t *r)
{
	return __atomic_load_n(&r->counter, __ATOMIC_ACQUIRE);
}
static inline void refcount_inc(refcount_t *r)
{
	if (__atomic_fetch_add(&r->counter, 1, __ATOMIC_ACQ_REL) <= 0)
		test_bug("refcount_inc on 0", __FILE__, __LINE__, __func__);
}
static inline int refcount_dec_and_test(refcount_t *r)
{
	int v = __atomic_sub_fetch(&r->counter, 1, __ATOMIC_ACQ_REL);

	if (v < 0)
		test_bug("refcount underflow", __FILE__, __LINE__, __func__);
	return v == 0;
}

/* ---- interrupt flag and spinlocks (per thread) ---- */
extern __thread uint64_t t_irqflag; /* 0x200 while "interrupts" are on */
extern __thread int t_spins_held;

static inline int irqs_disabled(void)
{
	return t_irqflag == 0;
}
static inline int irqs_enabled(void)
{
	return t_irqflag != 0;
}
static inline uint64_t local_irq_save(void)
{
	uint64_t f = t_irqflag;

	t_irqflag = 0;
	return f;
}
static inline void local_irq_restore(uint64_t f)
{
	t_irqflag = f & 0x200;
}

typedef struct spinlock {
	volatile uint32_t locked;
	volatile uint32_t owner_cpu;
	const char *name;
	void *owner; /* the holding thread's task */
} spinlock_t;

#define SPINLOCK_INIT(n)                                                  \
	{                                                                 \
		.locked = 0, .owner_cpu = 0xFFFFFFFF, .name = (n),        \
		.owner = NULL                                             \
	}

struct task;
struct task *kshim_current_or_null(void);

static inline void spinlock_init(spinlock_t *l, const char *name)
{
	l->locked = 0;
	l->owner_cpu = 0xFFFFFFFF;
	l->name = name;
	l->owner = NULL;
}

static inline void spin_lock(spinlock_t *l)
{
	unsigned long spins = 0;

	if (l->locked && l->owner == (void *)kshim_current_or_null() &&
	    l->owner)
		test_bug("spin_lock recursion (deadlock)", __FILE__, __LINE__,
			 l->name);
	while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
		if (++spins > 4000000000ul)
			test_bug("spinlock stuck", __FILE__, __LINE__, l->name);
		h_yield();
	}
	l->owner = kshim_current_or_null();
	t_spins_held++;
}

static inline int spin_trylock(spinlock_t *l)
{
	if (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
		return 0;
	l->owner = kshim_current_or_null();
	t_spins_held++;
	return 1;
}

static inline void spin_unlock(spinlock_t *l)
{
	if (!l->locked)
		test_bug("spin_unlock of a free lock", __FILE__, __LINE__,
			 l->name);
	l->owner = NULL;
	t_spins_held--;
	__atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

static inline int spin_is_locked(spinlock_t *l)
{
	return l->locked != 0;
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

#define lockdep_assert_held(lock)                                         \
	do {                                                              \
		if (unlikely(!(lock)->locked))                            \
			test_bug("lockdep_assert_held " #lock, __FILE__,  \
				 __LINE__, __func__);                     \
	} while (0)

void bug_report_atomic_sleep(const char *file, int line, const char *func);
#define might_sleep()                                                     \
	do {                                                              \
		if (unlikely(irqs_disabled() || t_spins_held))            \
			bug_report_atomic_sleep(__FILE__, __LINE__,       \
						__func__);                \
	} while (0)

/* ---- tasks ---- */
typedef void (*task_entry_t)(void *arg);
typedef struct task {
	uint64_t id;
	char comm[64];
	volatile int gone;
	task_entry_t entry;
	void *arg;
} task_t;
#define SCHED_MIN_NICE (-20)
task_t *sched_current(void);
task_t *sched_add_task(task_entry_t entry, void *arg, void *stack,
		       size_t size);
task_t *sched_find_task_by_id(uint32_t pid);
void sched_set_user_nice(task_t *t, int nice);
void sched_yield_in_kernel(void);

/* ---- wait queues: one global sequence ---- */
struct wait_queue_head {
	const char *name;
};
#define WAIT_QUEUE_HEAD_INIT(n)                                           \
	{                                                                 \
		.name = (n)                                               \
	}
#define DECLARE_WAIT_QUEUE_HEAD(name)                                     \
	struct wait_queue_head name = WAIT_QUEUE_HEAD_INIT(#name)
static inline void wq_head_init(struct wait_queue_head *h, const char *n)
{
	h->name = n;
}
#define wake_up(h) ((void)(h), h_wq_wake())
#define wake_up_all(h) wake_up(h)
#define wait_event(wq_head, condition)                                    \
	do {                                                              \
		(void)(wq_head);                                          \
		might_sleep();                                            \
		for (;;) {                                                \
			unsigned long __s = h_wq_seq();                   \
			if (condition)                                    \
				break;                                    \
			h_wq_wait(__s);                                   \
		}                                                         \
	} while (0)

/* ---- mutexes ---- */
struct mutex {
	volatile int locked;
	void *owner;
	const char *name;
};
#define MUTEX_INITIALIZER(n)                                              \
	{                                                                 \
		.locked = 0, .owner = NULL, .name = (n)                   \
	}
#define DEFINE_MUTEX(var) struct mutex var = MUTEX_INITIALIZER(#var)
void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
int mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
static inline int mutex_is_locked(const struct mutex *m)
{
	return m->locked;
}
#define mutex_assert_held(m)                                              \
	do {                                                              \
		if (unlikely(!(m)->locked ||                              \
			     (m)->owner != (void *)sched_current()))      \
			test_bug("mutex_assert_held " #m, __FILE__,       \
				 __LINE__, __func__);                     \
	} while (0)

/* ---- ACPI tables (acpi.h) and PCI (pci.h), the bits acpi_dev.c uses ---- */
typedef struct {
	uint8_t bus_irq;
	uint32_t gsi;
	uint8_t polarity;
	uint8_t trigger_mode;
} irq_override_t;
int acpi_namespace_ready(void);
irq_override_t *acpi_get_irq_override(uint8_t isa_irq);

#ifndef KSHIM_REAL_PCI
typedef struct pci_device {
	unsigned char bus;
	unsigned char device;
	unsigned char function;
	unsigned short vendor_id;
	unsigned short device_id;
	uint32_t segment;
	struct pci_device *bus_self;
} pci_device_t;
#endif

/* ---- test helpers (kshim.c) ---- */
#define T_ASSERT(c)                                                       \
	do {                                                              \
		if (!(c))                                                 \
			test_bug("assertion " #c, __FILE__, __LINE__,     \
				 __func__);                               \
	} while (0)
#define T_EQ(a, b)                                                        \
	do {                                                              \
		long long __a = (long long)(a), __b = (long long)(b);     \
		if (__a != __b) {                                         \
			kprintf("  %s = %lld, expected %s = %lld\n", #a,  \
				__a, #b, __b);                            \
			test_bug("T_EQ", __FILE__, __LINE__, __func__);   \
		}                                                         \
	} while (0)
/* Run a test case, count it. */
void test_case(const char *name, void (*fn)(void));
void test_summary_and_exit(void);
/* Make the calling thread a "task" (the main thread at start). */
void kshim_init_main(void);

#endif /* HOST_KSHIM_PRELUDE_H */
