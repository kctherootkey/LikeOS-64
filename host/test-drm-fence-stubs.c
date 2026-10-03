/* Kernel services the fence library calls, for the host test.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/dev/gpu/drm.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/timer.h>
#include <kernel/io/console.h>

extern void *malloc(unsigned long);
extern void abort(void);
extern int vprintf(const char *, __builtin_va_list);
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);

uint64_t test_irq_flag = 0x200;
int test_locks_held;
long test_allocs; /* live allocations */
uint64_t test_now = 1000;
int test_warnings;

void test_fatal(const char *what, const char *name)
{
	kprintf("FATAL: %s (%s)\n", what, name ? name : "?");
	abort();
}

/* Allocations carry a header; a freed block is poisoned and kept, never
 * reused, so a double free is caught at once and a write after free when
 * test_check_quarantine() looks at the poison. */
#define LIVE_MAGIC 0x4c49564531ULL
#define DEAD_MAGIC 0x4445414431ULL
#define POISON 0x6b

struct test_hdr {
	uint64_t magic;
	uint64_t size;
	struct test_hdr *next_dead;
	uint64_t pad;
};

static struct test_hdr *dead_list;

void *kalloc(size_t size)
{
	struct test_hdr *h = malloc(sizeof(*h) + (size ? size : 1));

	if (!h)
		return NULL;
	h->magic = LIVE_MAGIC;
	h->size = size;
	h->next_dead = NULL;
	memset(h + 1, 0xa5, size); /* not zeroed: catch missing initialisation */
	test_allocs++;
	return h + 1;
}

void *kcalloc(size_t count, size_t size)
{
	void *p = kalloc(count * size);

	if (p)
		memset(p, 0, count * size);
	return p;
}

void kfree(void *ptr)
{
	struct test_hdr *h;

	if (!ptr)
		return;
	h = (struct test_hdr *)ptr - 1;
	if (h->magic != LIVE_MAGIC)
		test_fatal(h->magic == DEAD_MAGIC ? "double free" : "free of a bad pointer", "kfree");
	h->magic = DEAD_MAGIC;
	memset(ptr, POISON, h->size);
	h->next_dead = dead_list;
	dead_list = h;
	test_allocs--;
}

void *krealloc(void *ptr, size_t new_size)
{
	void *p = kalloc(new_size);

	if (!p)
		return NULL;
	if (ptr) {
		struct test_hdr *h = (struct test_hdr *)ptr - 1;
		memcpy(p, ptr, h->size < new_size ? h->size : new_size);
		kfree(ptr);
	}
	return p;
}

int test_check_quarantine(void)
{
	int bad = 0;

	for (struct test_hdr *h = dead_list; h; h = h->next_dead) {
		const unsigned char *b = (const unsigned char *)(h + 1);
		for (uint64_t i = 0; i < h->size; i++) {
			if (b[i] != POISON) {
				kprintf("write after free: block of %llu bytes, offset %llu\n",
					(unsigned long long)h->size, (unsigned long long)i);
				bad++;
				break;
			}
		}
	}
	return bad;
}

void mm_memset(void *dest, int val, size_t len)
{
	memset(dest, val, len);
}

void mm_memcpy(void *dest, const void *src, size_t len)
{
	memcpy(dest, src, len);
}

int kprintf(const char *fmt, ...)
{
	__builtin_va_list ap;
	int n;

	if (fmt[0] == 'W' && fmt[1] == 'A')
		test_warnings++;
	__builtin_va_start(ap, fmt);
	n = vprintf(fmt, ap);
	__builtin_va_end(ap);
	return n;
}

int ksnprintf(char *buffer, size_t size, const char *format, ...)
{
	__builtin_va_list ap;
	int n;

	__builtin_va_start(ap, format);
	n = vsnprintf(buffer, size, format, ap);
	__builtin_va_end(ap);
	return n;
}

uint64_t hrtimer_now_ns(void)
{
	return ++test_now;
}

void hrtimer_init(hrtimer_t *t, hrtimer_fn_t fn, void *arg)
{
	(void)t;
	(void)fn;
	(void)arg;
}

void hrtimer_start(hrtimer_t *t, uint64_t abs_ns)
{
	(void)t;
	(void)abs_ns;
}

int hrtimer_cancel(hrtimer_t *t)
{
	(void)t;
	return 0;
}

int hrtimer_is_highres(void)
{
	return 1;
}

uint64_t timer_ticks(void)
{
	return test_now / 1000000;
}

static task_t test_task = { .id = 1, .tgid = 1, .comm = "test" };

task_t *sched_current(void)
{
	return &test_task;
}

int sched_claim_wake(task_t *t, task_state_t from)
{
	if (t->state != from)
		return 0;
	t->state = TASK_READY;
	return 1;
}

void sched_enqueue_ready(task_t *t)
{
	(void)t;
}

void sched_schedule(void)
{
	test_fatal("a test path slept", "sched_schedule");
}

int signal_pending(struct task *task)
{
	(void)task;
	return 0;
}

int test_poll_notified;

void poll_notify_wq(struct wait_queue_head *h)
{
	(void)h;
	test_poll_notified++;
}
