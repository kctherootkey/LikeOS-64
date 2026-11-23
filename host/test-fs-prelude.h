/* Host test prelude for the filesystem harness (host/test-fs.sh).
 *
 * Force-included (-include) into every kernel-side unit of the harness:
 * the driver under test, the real page cache, inode cache and dentry cache
 * (kernel/fs/pagecache.c, icache.c, dcache.c), the stubs and the runner.
 *
 * The kernel's own headers are used as they are -- task_t, vfs_mount_t,
 * block_device_t and the cache structures keep their real layout -- except
 * for the handful of inline functions that execute instructions a user
 * process may not (cli/popfq in the interrupt-flag helpers) or that would
 * spin for ever in a single-threaded program (spin_lock).  Those are
 * renamed out of the way while the headers are read and replaced below
 * with host versions that simulate the interrupt flag and turn every
 * misuse into a fatal test failure:
 *
 *   - spin_lock on a held lock: the harness runs one thread, so contention
 *     can only be recursion, which is a deadlock in the kernel;
 *   - spin_unlock of a free lock;
 *   - sleeping (sched_schedule on a blocked task, mutex_lock, wait_event)
 *     with a spinlock held or interrupts disabled;
 *   - sleeping at all on something no one else could ever release (one
 *     thread: a blocked sleep never ends -- reported as a deadlock).
 *
 * BUG()/BUG_ON() print the kernel message and a host stack trace and abort
 * instead of executing ud2.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_TEST_FS_PRELUDE_H
#define HOST_TEST_FS_PRELUDE_H

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

/* ---- harness services (test-fs-stubs.c) ---- */
void test_fatal(const char *what, const char *name)
	__attribute__((noreturn));
void test_fatalf(const char *fmt, ...)
	__attribute__((noreturn, format(printf, 1, 2)));
extern uint64_t test_irq_flag; /* 0x200 while "interrupts" are on */
extern int test_spins_held; /* spinlocks held right now */


/* ---- host side (test-fs-bdev.c, compiled against the C library) ----
 * Plain C types only: the kernel and the C library disagree about what
 * uint64_t is, so the two worlds never share a header. */
int hbd_open(const char *path, unsigned sector, int ro, int cow);
void hbd_close(void);
unsigned long long hbd_sectors(void);
unsigned hbd_sector_size(void);
int hbd_is_ro(void);
int hbd_read(unsigned long long lba, unsigned long long count, void *buf);
int hbd_write(unsigned long long lba, unsigned long long count,
	      const void *buf);
int hbd_flush(void);
int hbd_discard(unsigned long long lba, unsigned long long count);
void hbd_set_trap(int on);
int hbd_get_trap(void);
void hbd_set_log(int on);
void hbd_log_clear(void);
void hbd_set_scribble(int on);
void hbd_fault(char kind, unsigned long long n);
int hbd_cut_active(void);
void hbd_counts(unsigned long long *reads, unsigned long long *writes,
		unsigned long long *discards, unsigned long long *flushes,
		unsigned long long *wsect, unsigned long long *dropped);
int hbd_log_dump(const char *path);
long hbd_log_find(char op, unsigned long long lba, unsigned long long count,
		  long from);
unsigned long hbd_log_len(void);
int hbd_load_patches(const char *path);
int hbd_save(const char *path);
void *host_read_file(const char *path, unsigned long *len);
int host_write_file(const char *path, const void *data, unsigned long len);
void host_flush_out(void);
void host_print_stack(void);
void *h_malloc(unsigned long n);
void *h_calloc(unsigned long n, unsigned long m);
void *h_realloc(void *p, unsigned long n);
void *h_aligned_alloc(unsigned long align, unsigned long n);
void h_free(void *p);
void *h_memset(void *p, int c, unsigned long n);
void *h_memcpy(void *d, const void *s, unsigned long n);
void *h_memmove(void *d, const void *s, unsigned long n);
int h_memcmp(const void *a, const void *b, unsigned long n);
unsigned long h_strlen(const char *s);
int h_strcmp(const char *a, const char *b);
int h_strncmp(const char *a, const char *b, unsigned long n);
char *h_strdup(const char *s);
unsigned long long h_strtoull(const char *s, char **end, int base);
long long h_strtoll(const char *s, char **end, int base);
int h_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int h_fprintf_err(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
int h_snprintf(char *b, unsigned long n, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
int h_vsnprintf(char *b, unsigned long n, const char *fmt,
		__builtin_va_list ap);
void h_abort(void) __attribute__((noreturn));
void h_exit(int code) __attribute__((noreturn));
char *h_getenv(const char *name);
void h_qsort(void *base, unsigned long n, unsigned long size,
	     int (*cmp)(const void *, const void *));
/* script input: next line (without newline) or NULL at the end */
char *h_read_line(void *fh, char *buf, int cap);
void *h_open_script(const char *path); /* "-" = stdin */
void h_close_script(void *fh);

/* ---- spinlocks: single-threaded, so contention == deadlock ---- */
static inline void spinlock_init(spinlock_t *l, const char *name)
{
	l->locked = 0;
	l->owner_cpu = 0xFFFFFFFF;
	l->name = name;
}

static inline void spin_lock(spinlock_t *l)
{
	if (l->locked)
		test_fatal("spin_lock of a lock already held (deadlock)",
			   l->name);
	l->locked = 1;
	l->owner_cpu = 0;
	test_spins_held++;
}

static inline int spin_trylock(spinlock_t *l)
{
	if (l->locked)
		return 0;
	l->locked = 1;
	l->owner_cpu = 0;
	test_spins_held++;
	return 1;
}

static inline void spin_unlock(spinlock_t *l)
{
	if (!l->locked)
		test_fatal("spin_unlock of a lock that is not held", l->name);
	l->owner_cpu = 0xFFFFFFFF;
	l->locked = 0;
	test_spins_held--;
}

static inline int spin_is_locked(spinlock_t *l)
{
	return l->locked != 0;
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
	extern uint64_t test_clock_ns;
	return test_clock_ns;
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
