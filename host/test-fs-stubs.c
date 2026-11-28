/* Kernel services for the filesystem harness (host/test-fs.sh).
 *
 * Everything a filesystem driver and the real page / inode / dentry caches
 * call that is not compiled from the kernel tree itself.  The kernel's own
 * mutex.c, waitq.c, completion.c, mm/rwsem.c and ke/cred.c ARE compiled
 * in; what is here is the layer under them: one simulated task, a
 * single-threaded scheduler that reports a sleep nobody can end as a
 * deadlock, a simulated clock, heap and page allocation on the host heap
 * (AddressSanitizer watches it), the block device glue to the file-backed
 * device in test-fs-bdev.c, and the few VFS entry points a driver uses.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/mm/vmscan.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>

/* ------------------------------------------------------------------ */
/* failure reporting                                                    */
/* ------------------------------------------------------------------ */

uint64_t test_irq_flag = 0x200;
int test_spins_held;
long test_warnings; /* WARNING lines the kernel code printed */
long test_bugs;
int test_quiet_klog; /* kernel log to stderr suppressed (fuzzing) */
int test_klog_prefix = 1;

void test_fatal(const char *what, const char *name)
{
	h_fprintf_err("\nFATAL: %s (%s)\n", what, name ? name : "?");
	host_print_stack();
	h_printf("FAIL fatal %s (%s)\n", what, name ? name : "?");
	host_flush_out();
	h_abort();
}

void test_fatalf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	__builtin_va_start(ap, fmt);
	h_vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	test_fatal(buf, NULL);
}

void test_bug(const char *cond, const char *file, int line, const char *func)
{
	test_bugs++;
	if (cond)
		h_fprintf_err("BUG: condition (%s) true at %s:%d in %s()!\n",
			      cond, file, line, func);
	else
		h_fprintf_err("BUG: kernel BUG at %s:%d in %s()!\n", file, line,
			      func);
	test_fatal("BUG", func);
}

void bug_report_atomic_sleep(const char *file, int line, const char *func)
{
	h_fprintf_err("BUG: sleeping function called from atomic context at "
		      "%s:%d %s() (spinlocks held %d, irq %s)\n",
		      file, line, func, test_spins_held,
		      test_irq_flag ? "on" : "off");
	test_fatal("might_sleep in atomic context", func);
}

void panic(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	__builtin_va_start(ap, fmt);
	h_vsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	test_fatal("panic", buf);
}

/* ------------------------------------------------------------------ */
/* kernel log                                                           */
/* ------------------------------------------------------------------ */

/* kvsnprintf/ksnprintf/kstr* are the kernel's own, extracted from
 * kernel/io/console.c by test-fs.sh (test-fs-kfmt.c). */
int kvprintf(const char *format, va_list args)
{
	char buf[2048];
	int n = kvsnprintf(buf, sizeof(buf), format, args);

	/* WARN/BUG lines are counted: a run that printed one fails unless
	 * the runner was told warnings are expected (fuzzing). */
	if (!kstrncmp(buf, "WARNING", 7) || !kstrncmp(buf, "BUG", 3))
		test_warnings++;
	if (!test_quiet_klog)
		h_fprintf_err("%s%s", test_klog_prefix ? "[k] " : "", buf);
	return n;
}

int kprintf(const char *format, ...)
{
	va_list ap;
	int n;

	__builtin_va_start(ap, format);
	n = kvprintf(format, ap);
	__builtin_va_end(ap);
	return n;
}

void klog_append(const char *str, int len)
{
	(void)str;
	(void)len;
}

void *kmemset(void *ptr, int value, size_t size)
{
	return h_memset(ptr, value, size);
}

void *kmemcpy(void *dest, const void *src, size_t size)
{
	return h_memcpy(dest, src, size);
}

int kmemcmp(const void *a, const void *b, size_t n)
{
	return h_memcmp(a, b, n);
}

void mm_memset(void *dest, int val, size_t len)
{
	h_memset(dest, val, len);
}

void mm_memcpy(void *dest, const void *src, size_t len)
{
	h_memcpy(dest, src, len);
}

/* ------------------------------------------------------------------ */
/* heap                                                                 */
/* ------------------------------------------------------------------ */

/* A header in front of every block: the size (krealloc needs it) and a
 * magic that catches a kfree of something kalloc never returned.  The
 * block is filled with a pattern, not zeroed, as the kernel's heap does
 * not zero either: code that forgets to initialise shows it. */
#define KH_LIVE 0x4b414c4c4f43ULL
#define KH_DEAD 0x4b46524545ULL

struct kh {
	uint64_t magic;
	uint64_t size;
};

long test_kalloc_live;
uint64_t test_kalloc_bytes, test_kalloc_peak;
uint64_t test_kalloc_max = 64ULL << 20; /* one allocation */
uint64_t test_kalloc_limit = 1024ULL << 20; /* everything live */
uint64_t test_kalloc_fail_at; /* 0 off: fail the Nth kalloc from now */
uint64_t test_kalloc_count;

void *kalloc(size_t size)
{
	struct kh *h;

	BUG_ON(size == 0); /* the slab allocator BUGs on it too */
	test_kalloc_count++;
	if (test_kalloc_fail_at && test_kalloc_count == test_kalloc_fail_at)
		return NULL;
	if (size > test_kalloc_max) {
		h_fprintf_err("test-fs: kalloc(%lu) refused: larger than the "
			      "harness limit %llu (uncapped allocation?)\n",
			      (unsigned long)size,
			      (unsigned long long)test_kalloc_max);
		return NULL;
	}
	if (test_kalloc_bytes + size > test_kalloc_limit) {
		h_fprintf_err("test-fs: kalloc(%lu) refused: %llu bytes live "
			      "(harness limit)\n",
			      (unsigned long)size,
			      (unsigned long long)test_kalloc_bytes);
		return NULL;
	}
	h = h_malloc(sizeof(*h) + size);
	if (!h)
		return NULL;
	h->magic = KH_LIVE;
	h->size = size;
	h_memset(h + 1, 0xa5, size);
	test_kalloc_live++;
	test_kalloc_bytes += size;
	if (test_kalloc_bytes > test_kalloc_peak)
		test_kalloc_peak = test_kalloc_bytes;
	return h + 1;
}

void kfree(void *ptr)
{
	struct kh *h;

	if (!ptr)
		return;
	h = (struct kh *)ptr - 1;
	if (h->magic != KH_LIVE)
		test_fatal(h->magic == KH_DEAD ? "kfree: double free" :
						 "kfree of a pointer kalloc did "
						 "not return",
			   "kfree");
	h->magic = KH_DEAD;
	test_kalloc_live--;
	test_kalloc_bytes -= h->size;
	h_free(h);
}

void *kcalloc(size_t count, size_t size)
{
	if (size && count > (size_t)-1 / size)
		return NULL;
	void *p = kalloc(count * size);
	if (p)
		h_memset(p, 0, count * size);
	return p;
}

void *krealloc(void *ptr, size_t new_size)
{
	struct kh *h;
	void *n;

	if (!ptr)
		return kalloc(new_size);
	if (new_size == 0) {
		kfree(ptr);
		return NULL;
	}
	h = (struct kh *)ptr - 1;
	if (h->magic != KH_LIVE)
		test_fatal("krealloc of a dead or foreign pointer", "krealloc");
	n = kalloc(new_size);
	if (!n)
		return NULL;
	h_memcpy(n, ptr, h->size < new_size ? h->size : new_size);
	kfree(ptr);
	return n;
}

void *kalloc_dma(size_t size)
{
	return kalloc(size);
}

void *kcalloc_dma(size_t count, size_t size)
{
	return kcalloc(count, size);
}

void kfree_dma(void *ptr)
{
	kfree(ptr);
}

uint64_t test_buffercache_bytes;

void mm_buffercache_account(long delta)
{
	test_buffercache_bytes += (uint64_t)delta;
}

uint64_t mm_buffercache_bytes(void)
{
	return test_buffercache_bytes;
}

/* In-memory file data (tmpfs), counted as the kernel counts it. */
int64_t test_shmem_pages;

void mm_shmem_account(long pages)
{
	test_shmem_pages += pages;
}

uint64_t mm_shmem_pages(void)
{
	return test_shmem_pages > 0 ? (uint64_t)test_shmem_pages : 0;
}

/* Reclaim (kernel/mm/vmscan.c): the free-memory levels are fixed here, the
 * shrinkers the code under test registers are kept, and the runner's
 * "shrink" command asks them for memory the way the reclaim thread does. */
static struct vm_shrinker *test_shrinkers;

uint64_t vm_wmark_min(void)
{
	return 256;
}

uint64_t vm_wmark_low(void)
{
	return 320;
}

uint64_t vm_wmark_high(void)
{
	return 384;
}

void vm_wake_reclaim(void)
{
}

void vm_check_free_memory(void)
{
}

/* Frame references, as kernel/mm/memory.c counts them: one from the
 * allocation, one more for every mapping.  Only frames with extra
 * references are recorded; the rest count one. */
#define TREF_SLOTS 4096
static struct {
	uint64_t phys;
	unsigned extra;
} tref[TREF_SLOTS];

static int tref_find(uint64_t phys, int create)
{
	int free_slot = -1;

	for (int i = 0; i < TREF_SLOTS; i++) {
		if (tref[i].phys == phys)
			return i;
		if (!tref[i].phys && free_slot < 0)
			free_slot = i;
	}
	if (!create || free_slot < 0)
		return -1;
	tref[free_slot].phys = phys;
	tref[free_slot].extra = 0;
	return free_slot;
}

void mm_get_page(uint64_t phys)
{
	int i = tref_find(phys, 1);

	if (i < 0)
		test_fatal("frame reference table full", "mm_get_page");
	tref[i].extra++;
}

void mm_put_page(uint64_t phys)
{
	int i = tref_find(phys, 0);

	if (i >= 0 && tref[i].extra) {
		if (--tref[i].extra == 0)
			tref[i].phys = 0;
		return;
	}
	mm_free_physical_page(phys);
}

uint16_t mm_get_page_refcount(uint64_t phys)
{
	int i = tref_find(phys, 0);

	return (uint16_t)(1 + (i >= 0 ? tref[i].extra : 0));
}

/* All the memory the page budget allows (--pages). */
extern uint64_t test_pages_limit;

uint64_t mm_get_usable_pages(void)
{
	return test_pages_limit;
}

void vm_register_shrinker(struct vm_shrinker *s)
{
	s->next = test_shrinkers;
	test_shrinkers = s;
}

void vm_unregister_shrinker(struct vm_shrinker *s)
{
	for (struct vm_shrinker **pp = &test_shrinkers; *pp; pp = &(*pp)->next)
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	s->next = NULL;
}

/* Ask every registered cache for up to `nr' objects; how many went. */
unsigned long test_vm_shrink(unsigned long nr)
{
	unsigned long freed = 0;

	for (struct vm_shrinker *s = test_shrinkers; s; s = s->next) {
		unsigned long n = s->count(s->arg);

		freed += s->scan(s->arg, n < nr ? n : nr);
	}
	return freed;
}

/* ------------------------------------------------------------------ */
/* physical pages                                                        */
/* ------------------------------------------------------------------ */

/* A page is host memory; its "physical address" is the address the
 * kernel's phys_to_virt() turns back into that pointer, so the direct map
 * is the identity on host addresses. */
/* No physical memory is direct-mapped here: a path buffer
 * (kernel/fs/pathbuf.c) gives its page back and comes from kalloc(). */
uint64_t g_direct_map_bytes;

uint64_t test_pages_live, test_pages_peak;
uint64_t test_pages_limit = 262144; /* 1 GB */
uint64_t test_free_pages_override; /* 0: limit - live */

static uint64_t host_to_phys(void *p)
{
	return (uint64_t)(uintptr_t)p - PHYS_MAP_BASE;
}

uint64_t mm_allocate_contiguous_pages(size_t page_count)
{
	if (!page_count || test_pages_live + page_count > test_pages_limit)
		return 0;
	void *p = h_aligned_alloc(4096, page_count * 4096);
	if (!p)
		return 0;
	h_memset(p, 0x5a, page_count * 4096);
	test_pages_live += page_count;
	if (test_pages_live > test_pages_peak)
		test_pages_peak = test_pages_live;
	uint64_t phys = host_to_phys(p);
	if (phys == 0)
		test_fatal("host page maps to physical 0", "pages");
	return phys;
}

void mm_free_contiguous_pages(uint64_t physical_address, size_t page_count)
{
	if (!physical_address)
		return;
	test_pages_live -= page_count;
	h_free(phys_to_virt(physical_address));
}

uint64_t mm_allocate_physical_page(void)
{
	return mm_allocate_contiguous_pages(1);
}

void mm_free_physical_page(uint64_t physical_address)
{
	mm_free_contiguous_pages(physical_address, 1);
}

uint64_t mm_allocate_physical_page_reclaim(void)
{
	uint64_t p = mm_allocate_physical_page();
	if (!p) {
		pagecache_shrink(64, 1);
		p = mm_allocate_physical_page();
	}
	return p;
}

uint64_t mm_get_free_pages(void)
{
	if (test_free_pages_override)
		return test_free_pages_override;
	return test_pages_limit > test_pages_live ?
		       test_pages_limit - test_pages_live :
		       0;
}

void mm_close_deferred(struct task *t)
{
	(void)t;
}

void smap_disable(void)
{
}

void smap_enable(void)
{
}

/* ------------------------------------------------------------------ */
/* user copies: the harness's "user" buffers are host memory            */
/* ------------------------------------------------------------------ */

bool validate_user_ptr(uint64_t ptr, size_t len)
{
	return ptr != 0 && ptr + len >= ptr;
}

int copy_from_user(void *kernel_dst, const void *user_src, size_t len)
{
	if (len && !validate_user_ptr((uint64_t)(uintptr_t)user_src, len))
		return -EFAULT;
	h_memcpy(kernel_dst, user_src, len);
	return 0;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t len)
{
	if (len && !validate_user_ptr((uint64_t)(uintptr_t)user_dst, len))
		return -EFAULT;
	h_memcpy(user_dst, kernel_src, len);
	return 0;
}

int copy_user_path(const char *user_path, char *kbuf, size_t kbuf_size)
{
	size_t i;

	for (i = 0; i + 1 < kbuf_size && user_path[i]; i++)
		kbuf[i] = user_path[i];
	if (user_path[i])
		return -ENAMETOOLONG;
	kbuf[i] = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* time                                                                 */
/* ------------------------------------------------------------------ */

/* Simulated and deterministic: the clock moves only when the harness
 * moves it (tick command, timed sleeps, yields). */
uint64_t test_clock_ns = 1000000000ULL;
uint64_t test_epoch_base = 1767225600ULL; /* 2026-01-01 00:00:00 UTC */
#define TEST_HZ 200ULL

uint64_t hrtimer_now_ns(void)
{
	return test_clock_ns;
}

uint64_t timer_get_precise_ns(void)
{
	return test_clock_ns;
}

uint64_t timer_get_precise_us(void)
{
	return test_clock_ns / 1000;
}

uint64_t timer_ticks(void)
{
	return test_clock_ns / (1000000000ULL / TEST_HZ);
}

uint64_t timer_get_ticks_at_cpu_tick(void)
{
	return timer_ticks();
}

uint32_t timer_get_frequency(void)
{
	return (uint32_t)TEST_HZ;
}

uint64_t timer_get_epoch(void)
{
	return test_epoch_base + test_clock_ns / 1000000000ULL;
}

uint64_t timer_get_boot_epoch(void)
{
	return test_epoch_base;
}

uint64_t timer_get_uptime(void)
{
	return test_clock_ns / 1000000000ULL;
}

uint64_t timer_get_realtime_ns(void)
{
	return test_epoch_base * 1000000000ULL + test_clock_ns;
}

uint64_t timer_realtime_offset_ns(uint64_t mono)
{
	(void)mono;
	return test_epoch_base * 1000000000ULL;
}

uint64_t timer_ns_to_ticks(uint64_t ns)
{
	return ns / (1000000000ULL / TEST_HZ);
}

uint64_t timer_us_to_ticks(uint64_t us)
{
	return us / (1000000ULL / TEST_HZ);
}

uint64_t timer_ms_to_ticks(uint64_t ms)
{
	return ms / (1000ULL / TEST_HZ);
}

uint64_t timer_s_to_ticks(uint64_t secs)
{
	return secs * TEST_HZ;
}

uint64_t timer_ticks_to_s(uint64_t ticks)
{
	return ticks / TEST_HZ;
}

uint64_t timer_us_per_tick(void)
{
	return 1000000ULL / TEST_HZ;
}

/* ------------------------------------------------------------------ */
/* the one task and the scheduler                                       */
/* ------------------------------------------------------------------ */

static task_t *test_task;
static uint64_t test_wait_deadline; /* hrtimer_wait_arm, 0 = none */
uint64_t test_yields; /* consecutive yields without other progress */
uint64_t test_yield_limit = 1000000;
int test_have_task = 1; /* 0: run as "kernel context" (no task) */

volatile uint32_t g_smp_up_mode = 0;
volatile int g_preempt_count = 0;
uint32_t g_cpu_features_ext = 0;
spinlock_t g_task_list_lock = SPINLOCK_INIT("task_list");
spinlock_t g_wait_lock = SPINLOCK_INIT("wait");

void test_task_init(uint32_t uid, uint32_t gid)
{
	if (!test_task) {
		test_task = h_calloc(1, sizeof(task_t));
		if (!test_task)
			test_fatal("out of memory", "task");
	}
	test_task->id = 100;
	test_task->tgid = 100;
	h_memcpy(test_task->comm, "test-fs", 8);
	test_task->state = TASK_RUNNING;
	test_task->fs_cwd = NULL; /* "/" (kernel/ke/task_path.h) */
	test_task->umask = 022;
	test_task->cred.uid = test_task->cred.euid = test_task->cred.suid =
		test_task->cred.fsuid = uid;
	test_task->cred.gid = test_task->cred.egid = test_task->cred.sgid =
		test_task->cred.fsgid = gid;
	test_task->cred.ngroups = 0;
}

void test_task_set_id(uint64_t id)
{
	test_task->id = id;
	test_task->tgid = (int)id;
}

void test_task_set_umask(uint32_t mask)
{
	test_task->umask = mask & 0777;
}

task_t *sched_current(void)
{
	return test_have_task ? test_task : NULL;
}

void *test_task_walk_slot(void)
{
	return &test_task->vfs_walk;
}

uint32_t task_umask(task_t *t)
{
	return t ? (t->umask & 0777) : 0022u;
}

uint32_t task_set_umask(task_t *t, uint32_t mask)
{
	uint32_t old = task_umask(t);
	if (t)
		t->umask = mask & 0777;
	return old;
}

int task_dumpable(task_t *t)
{
	(void)t;
	return 1;
}

void task_set_dumpable(task_t *t, int dumpable)
{
	(void)t;
	(void)dumpable;
}

bool task_ptr_ok(const task_t *t)
{
	return t == test_task;
}

int signal_pending(struct task *task)
{
	(void)task;
	return 0;
}

/* Sleeping in a single-threaded harness: a timed sleep elapses at once
 * (the clock jumps to its deadline); an untimed one could only be ended
 * by another task, and there is none -- a deadlock in the kernel. */
void sched_schedule(void)
{
	task_t *t = test_task;

	if (test_spins_held)
		test_fatalf("schedule with %d spinlock(s) held",
			    test_spins_held);
	if (!test_irq_flag)
		test_fatal("schedule with interrupts disabled", "sched");
	if (t->state != TASK_BLOCKED) {
		t->state = TASK_RUNNING;
		return;
	}
	if (test_wait_deadline) {
		if (test_wait_deadline > test_clock_ns)
			test_clock_ns = test_wait_deadline;
		t->state = TASK_RUNNING;
		return;
	}
	h_fprintf_err("\nDEADLOCK: the only task sleeps on channel %p "
		      "(nobody can wake it; a lock taken twice?)\n",
		      t->wait_channel);
	test_fatal("deadlock: untimed sleep in a single-threaded run",
		   "sched_schedule");
}

void sched_yield_in_kernel(void)
{
	if (test_spins_held)
		test_fatalf("yield with %d spinlock(s) held", test_spins_held);
	test_clock_ns += 1000000ULL;
	if (++test_yields > test_yield_limit)
		test_fatal("livelock: endless yield loop (waiting for "
			   "something no other task will do)",
			   "sched_yield_in_kernel");
}

void sched_wake_channel(void *channel)
{
	task_t *t = test_task;

	if (t && t->state == TASK_BLOCKED && t->wait_channel == channel)
		t->state = TASK_READY;
}

int sched_wake_channel_once(void *channel, int max)
{
	(void)max;
	sched_wake_channel(channel);
	return 0;
}

int sched_claim_wake(task_t *t, task_state_t from)
{
	task_state_t exp = from;

	return __atomic_compare_exchange_n(&t->state, &exp, TASK_READY, 0,
					   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

void sched_enqueue_ready(task_t *task)
{
	(void)task;
}

void hrtimer_wait_arm(struct task *task, uint64_t abs_ns)
{
	(void)task;
	test_wait_deadline = abs_ns;
}

void hrtimer_wait_disarm(struct task *task)
{
	(void)task;
	test_wait_deadline = 0;
}

int hrtimer_sleep_until(uint64_t abs_ns, uint64_t *remaining_ns)
{
	if (test_spins_held)
		test_fatal("sleep with a spinlock held", "hrtimer_sleep_until");
	if (abs_ns > test_clock_ns)
		test_clock_ns = abs_ns;
	if (remaining_ns)
		*remaining_ns = 0;
	return 0;
}

/* Kernel threads are recorded, never run: the harness drives writeback
 * itself (the runner's wb/tick commands). */
static int test_kthreads;

task_t *sched_add_task(task_entry_t entry, void *arg, void *stack_mem,
		       size_t stack_size)
{
	(void)entry;
	(void)arg;
	(void)stack_mem;
	(void)stack_size;
	static task_t *dummy[8];
	if (test_kthreads >= 8)
		return NULL;
	dummy[test_kthreads] = h_calloc(1, sizeof(task_t));
	return dummy[test_kthreads++];
}

/* ------------------------------------------------------------------ */
/* block device glue                                                    */
/* ------------------------------------------------------------------ */

block_device_t test_bdev;
int test_bdev_discard = 1; /* the device supports discard */
long test_io_errors; /* driver-visible I/O errors returned */
long test_failures; /* harness-level failures (runner counts them) */

static int bd_status(int r, const char *what, unsigned long lba,
		     unsigned long count)
{
	if (r == 0)
		return ST_OK;
	test_io_errors++;
	if (r == -2) {
		h_fprintf_err("test-fs: %s beyond the end of the device: lba "
			      "%lu count %lu (device %lu sectors)\n",
			      what, lba, count,
			      (unsigned long)test_bdev.total_sectors);
		if (what[0] == 'w' || what[0] == 'd') {
			h_printf("FAIL %s beyond device end lba=%lu count=%lu\n",
				 what, lba, count);
			test_failures++;
		}
		return ST_IO;
	}
	return ST_IO;
}

static int tb_read(block_device_t *dev, unsigned long lba,
		   unsigned long count, void *buf)
{
	(void)dev;
	if (test_spins_held)
		test_fatalf("block read with %d spinlock(s) held",
			    test_spins_held);
	return bd_status(hbd_read(lba, count, buf), "read", lba, count);
}

static int tb_write(block_device_t *dev, unsigned long lba,
		    unsigned long count, const void *buf)
{
	(void)dev;
	if (test_spins_held)
		test_fatalf("block write with %d spinlock(s) held",
			    test_spins_held);
	return bd_status(hbd_write(lba, count, buf), "write", lba, count);
}

static int tb_sync(block_device_t *dev)
{
	(void)dev;
	return hbd_flush() ? ST_IO : ST_OK;
}

void test_bdev_setup(const char *name, int ro)
{
	h_memset(&test_bdev, 0, sizeof(test_bdev));
	test_bdev.name = name;
	test_bdev.sector_size = hbd_sector_size();
	test_bdev.total_sectors = (unsigned long)hbd_sectors();
	test_bdev.read = tb_read;
	test_bdev.write = tb_write;
	test_bdev.sync = tb_sync;
	test_bdev.magic = BLOCK_DEVICE_MAGIC;
	test_bdev.flags = BDEV_F_PARTITION | BDEV_F_REGISTERED;
	test_bdev.physical_sector_size = test_bdev.sector_size;
	test_bdev.io_min = test_bdev.sector_size;
	test_bdev.max_sectors = 2048;
	test_bdev.discard_granularity =
		test_bdev_discard ? test_bdev.sector_size : 0;
	test_bdev.discard_max_sectors = test_bdev_discard ? 1u << 22 : 0;
	test_bdev.ro = (uint8_t)(ro ? 1 : 0);
	test_bdev.removable = 1;
	test_bdev.ready = 1;
	test_bdev.flush_supported = 1;
	refcount_set(&test_bdev.refcount, 1);
	test_bdev.registry_index = 0;
	test_bdev.major = 8;
	test_bdev.minor = 1;
}

int blk_bdev_read_only(const block_device_t *bdev)
{
	if (!bdev)
		return 1;
	if (!blk_bdev_is_extended(bdev))
		return 0;
	return bdev->ro ? 1 : 0;
}

int blk_st_to_errno(int st)
{
	switch (st) {
	case ST_OK:
		return 0;
	case ST_NOMEM:
		return -ENOMEM;
	case ST_INVALID:
		return -EINVAL;
	case ST_ROFS:
		return -EROFS;
	case ST_BUSY:
		return -EBUSY;
	case ST_NO_DEVICE:
		return -ENODEV;
	case ST_TIMEOUT:
		return -ETIMEDOUT;
	default:
		return -EIO;
	}
}

static int blk_check(block_device_t *bdev, uint64_t lba, uint64_t count)
{
	if (!bdev)
		return -EINVAL;
	if (lba + count < lba || lba + count > bdev->total_sectors)
		return -EINVAL;
	return 0;
}

int blk_read_sync(block_device_t *bdev, uint64_t lba, uint32_t count,
		  void *buf)
{
	int r = blk_check(bdev, lba, count);

	might_sleep();
	if (r)
		return r;
	return blk_st_to_errno(bdev->read(bdev, lba, count, buf));
}

int blk_read_sync_flags(block_device_t *bdev, uint64_t lba, uint32_t count,
			void *buf, uint32_t flags)
{
	(void)flags;
	return blk_read_sync(bdev, lba, count, buf);
}

int blk_write_sync(block_device_t *bdev, uint64_t lba, uint32_t count,
		   const void *buf, uint32_t flags)
{
	int r = blk_check(bdev, lba, count);

	might_sleep();
	if (r)
		return r;
	r = blk_st_to_errno(bdev->write(bdev, lba, count, buf));
	if (!r && (flags & BLK_REQ_FUA))
		hbd_flush();
	return r;
}

int blk_flush_sync(block_device_t *bdev)
{
	if (!bdev)
		return -EINVAL;
	might_sleep();
	return bdev->sync ? blk_st_to_errno(bdev->sync(bdev)) : 0;
}

int blk_discard_sync(block_device_t *bdev, uint64_t lba, uint64_t count,
		     int secure)
{
	(void)secure;
	if (!bdev)
		return -EINVAL;
	if (count == 0)
		return 0;
	might_sleep();
	if (!blk_bdev_is_extended(bdev) || !bdev->discard_max_sectors)
		return -EOPNOTSUPP;
	if (lba + count < lba || lba + count > bdev->total_sectors) {
		h_printf("FAIL discard beyond device end lba=%llu count=%llu\n",
			 (unsigned long long)lba, (unsigned long long)count);
		test_failures++;
		return -EINVAL;
	}
	if (bdev != &test_bdev)
		return -EOPNOTSUPP;
	int r = hbd_discard(lba, count);
	return r == 0 ? 0 : (r == -2 ? -EINVAL : -EIO);
}

int blk_zeroout_sync(block_device_t *bdev, uint64_t lba, uint64_t count,
		     uint32_t flags)
{
	(void)flags;
	int r = blk_check(bdev, lba, count);
	uint8_t *z;

	if (r)
		return r;
	z = kcalloc(64, bdev->sector_size);
	if (!z)
		return -ENOMEM;
	while (count && !r) {
		uint32_t n = count > 64 ? 64 : (uint32_t)count;
		r = blk_st_to_errno(bdev->write(bdev, lba, n, z));
		lba += n;
		count -= n;
	}
	kfree(z);
	return r;
}

void blk_bdev_get(block_device_t *bdev)
{
	refcount_inc(&bdev->refcount);
}

void blk_bdev_put(block_device_t *bdev)
{
	if (refcount_dec_and_test(&bdev->refcount))
		test_fatal("block device reference dropped to zero", bdev->name);
}

void blk_bdev_put_any(block_device_t *bdev)
{
	blk_bdev_put(bdev);
}

int blk_bdev_claim(block_device_t *bdev, void *holder)
{
	if (bdev->holder && bdev->holder != holder)
		return -EBUSY;
	bdev->holder = holder;
	bdev->flags |= BDEV_F_EXCL;
	return 0;
}

void blk_bdev_unclaim(block_device_t *bdev, void *holder)
{
	if (bdev->holder == holder) {
		bdev->holder = NULL;
		bdev->flags &= ~BDEV_F_EXCL;
	}
}

int block_sync(block_device_t *dev)
{
	return dev && dev->sync ? dev->sync(dev) : ST_OK;
}

/* CRC32 (IEEE 802.3, reflected, poly 0xEDB88320): start with crc = 0,
 * pre- and post-inverted, as the block layer's. */
uint32_t blk_crc32(uint32_t crc, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

/* ------------------------------------------------------------------ */
/* VFS entry points a driver calls                                      */
/* ------------------------------------------------------------------ */

vfs_mount_t *test_mount_ptr;
/* The root superblock pointer lives in the FAT driver (fat32/inode.c); any other driver gets it
 * here (the harness never registers a root). */
#ifndef TEST_HAVE_ROOT_SB
vfs_superblock_t *g_root_sb;
#endif
int test_root_registered;

vfs_mount_t *vfs_mnt_root(void)
{
	return test_mount_ptr;
}

int vfs_register_root(const vfs_ops_t *ops, void *fs_data,
		      struct vfs_superblock *sb, const vfs_fstype_t *type,
		      const char *fstype)
{
	(void)ops;
	(void)fs_data;
	(void)sb;
	(void)type;
	(void)fstype;
	test_root_registered++;
	return ST_OK;
}

/* As kernel/fs/mount.c's: the shared caches let go of the instance. */
void vfs_sb_release_caches(struct vfs_superblock *sb)
{
	if (!sb)
		return;
	pagecache_invalidate_sb(sb);
	icache_invalidate_sb(sb);
	dcache_invalidate_owner(sb);
}

/* As kernel/fs/mount.c's option walker. */
int vfs_next_option(const char **cursor, char *tmp, unsigned cap,
		    const char **key, const char **val)
{
	const char *p = *cursor;

	if (!p || cap < 2)
		return 0;
	while (*p == ',')
		p++;
	if (!*p) {
		*cursor = p;
		return 0;
	}
	unsigned n = 0;
	while (*p && *p != ',') {
		if (n + 1 < cap)
			tmp[n++] = *p;
		p++;
	}
	tmp[n] = '\0';
	*cursor = p;
	*key = tmp;
	*val = NULL;
	for (unsigned i = 0; i < n; i++)
		if (tmp[i] == '=') {
			tmp[i] = '\0';
			*val = tmp + i + 1;
			break;
		}
	return 1;
}

void vfs_meta_changed(void)
{
}

/* vfs_link_hop is the runner's (test-fs-main.c): it follows the link the
 * way the VFS walk does. */
