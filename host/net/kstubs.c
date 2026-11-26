/* Kernel services for one stack instance of the network harness
 * (host/test-net.sh).
 *
 * Compiled into EVERY instance, so each instance has its own heap
 * accounting, random generator, softirq table, task and interrupt flag
 * (the instance's global symbols are renamed after linking).  Time is the
 * simulator's clock (sim.c), shared by all instances; it moves only when
 * the simulator moves it.
 *
 * What is here is the layer under kernel/net: a checked heap with fault
 * injection, a seeded random generator, the timer API on the simulated
 * clock at a configurable frequency, the softirq table with a
 * deterministic pump in place of ksoftirqd, one task whose sleeps run the
 * simulation until it is woken, and the few entry points of other
 * subsystems the network code calls.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/net/net.h>
#include <kernel/net/softirq.h>
#include <kernel/mm/slab.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/dev/rand/random.h>
#include <kernel/ke/cred.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/uaccess.h>
#include <kernel/dev/nic/e1000.h>
#include <kernel/dev/nic/e1000e.h>
#include <kernel/dev/nic/rtl8139.h>
#include <kernel/dev/nic/pcnet32.h>
#include <kernel/dev/nic/ne2k.h>
#include <kernel/dev/nic/vmxnet3.h>
#include <kernel/dev/nic/eepro100.h>
#include <kernel/dev/nic/igb.h>
#include <kernel/dev/nic/tulip.h>
#include "kstubs.h"

int g_inst = -1; /* which instance this is (glue.c sets it) */

/* ------------------------------------------------------------------ */
/* failure reporting                                                    */
/* ------------------------------------------------------------------ */

uint64_t test_irq_flag = 0x200;
int test_spins_held;
uint64_t test_trylock_fails;
uint64_t test_warnings;
int test_quiet;
int test_in_softirq;

void test_fatal(const char *what, const char *name)
{
	char buf[512];

	ksnprintf(buf, sizeof(buf), "FATAL [n%d]: %s (%s)\n", g_inst, what,
		  name ? name : "?");
	hs_klog(-1, buf);
	hs_backtrace();
	hs_abort();
}

void test_fatalf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	__builtin_va_start(ap, fmt);
	kvsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	test_fatal(buf, NULL);
}

void test_bug(const char *cond, const char *file, int line, const char *func)
{
	char buf[512];

	if (cond)
		ksnprintf(buf, sizeof(buf),
			  "BUG: condition (%s) true at %s:%d in %s()!\n", cond,
			  file, line, func);
	else
		ksnprintf(buf, sizeof(buf), "BUG: kernel BUG at %s:%d in %s()!\n",
			  file, line, func);
	hs_klog(-1, buf);
	test_fatal("BUG", func);
}

void bug_report_atomic_sleep(const char *file, int line, const char *func)
{
	test_fatalf("sleeping function called from atomic context at %s:%d "
		    "%s() (spinlocks held %d, irq %s)",
		    file, line, func, test_spins_held,
		    test_irq_flag ? "on" : "off");
}

void panic(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	__builtin_va_start(ap, fmt);
	kvsnprintf(buf, sizeof(buf), fmt, ap);
	__builtin_va_end(ap);
	test_fatal("panic", buf);
}

/* ------------------------------------------------------------------ */
/* kernel log                                                           */
/* ------------------------------------------------------------------ */

int kvprintf(const char *format, va_list args)
{
	char buf[2048];
	int n = kvsnprintf(buf, sizeof(buf), format, args);

	/* WARN/BUG lines are counted: the tests fail a run that printed
	 * one unless it was expected. */
	if (!kstrncmp(buf, "WARNING", 7) || !kstrncmp(buf, "BUG", 3))
		test_warnings++;
	if (!test_quiet)
		hs_klog(g_inst, buf);
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
	return hs_memset(ptr, value, size);
}

void *kmemcpy(void *dest, const void *src, size_t size)
{
	return hs_memcpy(dest, src, size);
}

int kmemcmp(const void *a, const void *b, size_t n)
{
	return hs_memcmp(a, b, n);
}

void mm_memset(void *dest, int val, size_t len)
{
	hs_memset(dest, val, len);
}

void mm_memcpy(void *dest, const void *src, size_t len)
{
	hs_memcpy(dest, src, len);
}

/* ------------------------------------------------------------------ */
/* heap: kalloc and slab_alloc share one checked heap                   */
/* ------------------------------------------------------------------ */

/* A header in front of every block: the size (krealloc needs it) and a
 * magic that catches a free of something the heap never returned.  The
 * block is filled with a pattern, not zeroed, as the kernel's heap does
 * not zero either: code that forgets to initialise shows it. */
#define KH_LIVE 0x4b414c4c4f43ULL
#define KH_DEAD 0x4b46524545ULL

struct kh {
	uint64_t magic;
	uint64_t size;
};

uint64_t test_kalloc_live, test_kalloc_bytes, test_kalloc_peak;
uint64_t test_kalloc_max = 64ULL << 20; /* one allocation */
uint64_t test_kalloc_limit = 1536ULL << 20; /* everything live */
uint64_t test_kalloc_count;
uint64_t test_fail_at; /* absolute count to fail at, 0 = off */
uint64_t test_fail_every;

void test_alloc_fail(uint64_t nth, uint64_t every)
{
	test_fail_at = nth ? test_kalloc_count + nth : 0;
	test_fail_every = every;
}

static int heap_inject_failure(void)
{
	if (!test_fail_at || test_kalloc_count < test_fail_at)
		return 0;
	if (test_kalloc_count == test_fail_at)
		return 1;
	return test_fail_every &&
	       (test_kalloc_count - test_fail_at) % test_fail_every == 0;
}

void *kalloc(size_t size)
{
	struct kh *h;

	BUG_ON(size == 0); /* the slab allocator BUGs on it too */
	test_kalloc_count++;
	if (heap_inject_failure())
		return NULL;
	if (size > test_kalloc_max) {
		test_fatalf("kalloc(%lu) larger than the harness limit "
			    "(uncapped allocation?)",
			    (unsigned long)size);
	}
	if (test_kalloc_bytes + size > test_kalloc_limit)
		return NULL;
	h = hs_malloc(sizeof(*h) + size);
	if (!h)
		return NULL;
	h->magic = KH_LIVE;
	h->size = size;
	hs_memset(h + 1, 0xa5, size);
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
	hs_free(h);
}

void *kcalloc(size_t count, size_t size)
{
	if (size && count > (size_t)-1 / size)
		return NULL;
	void *p = kalloc(count * size);
	if (p)
		hs_memset(p, 0, count * size);
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
	hs_memcpy(n, ptr, h->size < new_size ? h->size : new_size);
	kfree(ptr);
	return n;
}

/* The slab allocator's rule the network code must keep: never with
 * interrupts disabled (a free can fire a TLB-shootdown IPI).  Counted;
 * fatal in strict mode. */
uint64_t test_slab_irqoff;
int test_slab_strict;

static void slab_context_check(const char *what)
{
	if (test_irq_flag != 0)
		return;
	test_slab_irqoff++;
	if (test_slab_strict)
		test_fatalf("%s with interrupts disabled", what);
}

void *slab_alloc(size_t size)
{
	slab_context_check("slab_alloc");
	return kalloc(size);
}

void slab_free(void *ptr)
{
	slab_context_check("slab_free");
	kfree(ptr);
}

void *slab_calloc(size_t count, size_t size)
{
	slab_context_check("slab_calloc");
	return kcalloc(count, size);
}

void *slab_realloc(void *ptr, size_t new_size)
{
	slab_context_check("slab_realloc");
	return krealloc(ptr, new_size);
}

void *kalloc_dma(size_t size)
{
	return kalloc(size);
}

void kfree_dma(void *ptr)
{
	kfree(ptr);
}

/* ------------------------------------------------------------------ */
/* packet buffers under AddressSanitizer                                */
/* ------------------------------------------------------------------ */

/* The skb pools are static arrays, so reading past the end of a packet
 * stays inside the pool and AddressSanitizer cannot see it.  The build
 * links every skb_alloc/skb_put call of the stack to these wrappers
 * (ld --wrap): the bytes of a buffer past what its allocation asked for
 * are poisoned until the buffer goes back to the pool. */
#include <kernel/net/skb.h>
sk_buff_t *__real_skb_alloc(uint32_t min_payload);
void __real_skb_put(sk_buff_t *skb);
sk_buff_t *__wrap_skb_alloc(uint32_t min_payload);
void __wrap_skb_put(sk_buff_t *skb);
#ifdef __SANITIZE_ADDRESS__
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#else
#define __asan_poison_memory_region(a, s) ((void)(a), (void)(s))
#define __asan_unpoison_memory_region(a, s) ((void)(a), (void)(s))
#endif

uint64_t test_skb_count, test_skb_fail_at, test_skb_fail_every;

void test_skb_fail(uint64_t nth, uint64_t every)
{
	test_skb_fail_at = nth ? test_skb_count + nth : 0;
	test_skb_fail_every = every;
}

sk_buff_t *__wrap_skb_alloc(uint32_t min_payload)
{
	test_skb_count++;
	if (test_skb_fail_at && test_skb_count >= test_skb_fail_at &&
	    (test_skb_count == test_skb_fail_at ||
	     (test_skb_fail_every &&
	      (test_skb_count - test_skb_fail_at) % test_skb_fail_every == 0)))
		return NULL;
	sk_buff_t *skb = __real_skb_alloc(min_payload);

	if (skb && skb->data + min_payload < skb->end)
		__asan_poison_memory_region(skb->data + min_payload,
					    (size_t)(skb->end - skb->data) -
						    min_payload);
	return skb;
}

void __wrap_skb_put(sk_buff_t *skb)
{
	if (skb && __atomic_load_n(&skb->refcount, __ATOMIC_ACQUIRE) == 1)
		__asan_unpoison_memory_region(skb->head,
					      (size_t)(skb->end - skb->head));
	__real_skb_put(skb);
}

/* ------------------------------------------------------------------ */
/* random: seeded, deterministic per instance                           */
/* ------------------------------------------------------------------ */

static uint64_t rng_s[4];
static int rng_seeded;

static uint64_t rotl64(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64_t splitmix64(uint64_t *x)
{
	uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static uint64_t rng_next(void)
{
	if (!rng_seeded) {
		uint64_t s = hs_seed(g_inst);

		for (int i = 0; i < 4; i++)
			rng_s[i] = splitmix64(&s);
		rng_seeded = 1;
	}
	uint64_t r = rotl64(rng_s[1] * 5, 7) * 9;
	uint64_t t = rng_s[1] << 17;

	rng_s[2] ^= rng_s[0];
	rng_s[3] ^= rng_s[1];
	rng_s[1] ^= rng_s[2];
	rng_s[0] ^= rng_s[3];
	rng_s[2] ^= t;
	rng_s[3] = rotl64(rng_s[3], 45);
	return r;
}

void random_init(void)
{
	rng_seeded = 0;
}

void random_add_entropy(const void *data, size_t len)
{
	(void)data;
	(void)len;
}

int random_get_bytes(void *buf, size_t len, int blocking)
{
	uint8_t *p = buf;

	(void)blocking;
	while (len) {
		uint64_t r = rng_next();
		for (int i = 0; i < 8 && len; i++, len--)
			*p++ = (uint8_t)(r >> (8 * i));
	}
	return 0;
}

uint32_t random_u32(void)
{
	return (uint32_t)(rng_next() >> 32);
}

uint64_t random_u64(void)
{
	return rng_next();
}

/* SipHash-2-4 (Aumasson/Bernstein), the keyed hash the ISN and the SYN
 * cookies use. */
#define SIPROUND                                                         \
	do {                                                             \
		v0 += v1;                                                \
		v1 = rotl64(v1, 13);                                     \
		v1 ^= v0;                                                \
		v0 = rotl64(v0, 32);                                     \
		v2 += v3;                                                \
		v3 = rotl64(v3, 16);                                     \
		v3 ^= v2;                                                \
		v0 += v3;                                                \
		v3 = rotl64(v3, 21);                                     \
		v3 ^= v0;                                                \
		v2 += v1;                                                \
		v1 = rotl64(v1, 17);                                     \
		v1 ^= v2;                                                \
		v2 = rotl64(v2, 32);                                     \
	} while (0)

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

int test_sip12_fixed;
uint32_t test_sip12_value;

uint64_t siphash_2_4(const uint8_t key[16], const void *data, size_t len)
{
	if (len == 12 && test_sip12_fixed)
		return test_sip12_value;

	const uint8_t *in = data;
	uint64_t k0 = le64(key), k1 = le64(key + 8);
	uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
	uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
	uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
	uint64_t v3 = 0x7465646279746573ULL ^ k1;
	uint64_t b = (uint64_t)len << 56;
	size_t i;

	for (i = 0; i + 8 <= len; i += 8) {
		uint64_t m = le64(in + i);
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}
	for (size_t j = 0; i + j < len; j++)
		b |= (uint64_t)in[i + j] << (8 * j);
	v3 ^= b;
	SIPROUND;
	SIPROUND;
	v0 ^= b;
	v2 ^= 0xff;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	return v0 ^ v1 ^ v2 ^ v3;
}

/* ------------------------------------------------------------------ */
/* time: the simulator's clock at the configured tick frequency         */
/* ------------------------------------------------------------------ */

static uint64_t hz(void)
{
	return hs_hz();
}

uint64_t timer_ticks(void)
{
	uint64_t ns = hs_now_ns();

	return (ns / 1000000000ULL) * hz() + (ns % 1000000000ULL) * hz() /
						     1000000000ULL;
}

uint64_t timer_get_ticks_at_cpu_tick(void)
{
	return timer_ticks();
}

uint32_t timer_get_frequency(void)
{
	return (uint32_t)hz();
}

uint64_t timer_get_precise_ns(void)
{
	return hs_now_ns();
}

uint64_t timer_get_precise_us(void)
{
	return hs_now_ns() / 1000ULL;
}

uint64_t hrtimer_now_ns(void)
{
	return hs_now_ns();
}

/* The conversions round up, as the kernel's (kernel/ke/timer.c). */
uint64_t timer_ns_to_ticks(uint64_t ns)
{
	uint64_t secs = ns / 1000000000ULL, frac = ns % 1000000000ULL;

	return secs * hz() + (frac * hz() + 999999999ULL) / 1000000000ULL;
}

uint64_t timer_us_to_ticks(uint64_t us)
{
	uint64_t secs = us / 1000000ULL, frac = us % 1000000ULL;

	return secs * hz() + (frac * hz() + 999999ULL) / 1000000ULL;
}

uint64_t timer_ms_to_ticks(uint64_t ms)
{
	uint64_t secs = ms / 1000ULL, frac = ms % 1000ULL;

	return secs * hz() + (frac * hz() + 999ULL) / 1000ULL;
}

uint64_t timer_s_to_ticks(uint64_t secs)
{
	return secs * hz();
}

uint64_t timer_ticks_to_s(uint64_t ticks)
{
	return ticks / hz();
}

uint64_t timer_us_per_tick(void)
{
	return 1000000ULL / hz();
}

uint64_t timer_get_uptime(void)
{
	return hs_now_ns() / 1000000000ULL;
}

uint64_t timer_get_epoch(void)
{
	return 1767225600ULL + hs_now_ns() / 1000000000ULL;
}

uint64_t timer_get_realtime_ns(void)
{
	return 1767225600ULL * 1000000000ULL + hs_now_ns();
}

/* ------------------------------------------------------------------ */
/* the instance's one task                                              */
/* ------------------------------------------------------------------ */

/* The test's calls into the socket layer run as this task.  When it
 * sleeps the simulation runs (hs_block) until something wakes it, so a
 * blocking call works as it does on the target; with nothing to wake it
 * the simulator reports the hang at its time limit. */
static task_t *test_task;
static uint64_t test_wait_deadline_ns; /* hrtimer_wait_arm */
uint64_t test_yields;

volatile uint32_t g_smp_up_mode = 0;
volatile int g_preempt_count = 0;

void test_task_init(void)
{
	if (!test_task) {
		test_task = hs_malloc(sizeof(task_t));
		if (!test_task)
			test_fatal("out of memory", "task");
	}
	hs_memset(test_task, 0, sizeof(task_t));
	test_task->id = 100 + (uint64_t)g_inst;
	test_task->tgid = 100 + g_inst;
	hs_memcpy(test_task->comm, "test-net", 9);
	test_task->state = TASK_RUNNING;
}

task_t *sched_current(void)
{
	return test_task;
}

int signal_pending(struct task *task)
{
	(void)task;
	return 0;
}

uint32_t current_euid(void)
{
	return 0;
}

int capable(void)
{
	return 1;
}

static int task_still_blocked(void)
{
	return test_task->state == TASK_BLOCKED;
}

void sched_schedule(void)
{
	task_t *t = test_task;
	uint64_t deadline = 0;

	if (test_spins_held)
		test_fatalf("schedule with %d spinlock(s) held",
			    test_spins_held);
	if (!test_irq_flag)
		test_fatal("schedule with interrupts disabled", "sched");
	if (test_in_softirq)
		test_fatal("schedule from softirq context", "sched");
	if (t->state != TASK_BLOCKED) {
		t->state = TASK_RUNNING;
		return;
	}
	if (t->wakeup_tick) {
		/* the kernel's sleeper sweep runs in the tick handler: a
		 * sleeper wakes at the first tick at or after wakeup_tick,
		 * never before the next tick */
		uint64_t tk = t->wakeup_tick;

		if (tk <= timer_ticks())
			tk = timer_ticks() + 1;
		deadline = (tk / hz()) * 1000000000ULL +
			   (tk % hz()) * 1000000000ULL / hz();
	}
	if (test_wait_deadline_ns &&
	    (!deadline || test_wait_deadline_ns < deadline))
		deadline = test_wait_deadline_ns;
	hs_block(g_inst, task_still_blocked, deadline);
	t->state = TASK_RUNNING;
	t->wait_channel = NULL;
}

void sched_yield_in_kernel(void)
{
	if (test_spins_held)
		test_fatalf("yield with %d spinlock(s) held", test_spins_held);
	if (test_in_softirq)
		test_fatal("yield from softirq context", "sched");
	if (++test_yields > 1000000)
		test_fatal("livelock: endless yield loop", "sched_yield");
	hs_yield(g_inst);
}

void sched_wake_channel(void *channel)
{
	task_t *t = test_task;

	if (t && t->state == TASK_BLOCKED && t->wait_channel == channel) {
		t->state = TASK_READY;
		t->wait_channel = NULL;
	}
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
	test_wait_deadline_ns = abs_ns;
}

void hrtimer_wait_disarm(struct task *task)
{
	(void)task;
	test_wait_deadline_ns = 0;
}

int hrtimer_sleep_until(uint64_t abs_ns, uint64_t *remaining_ns)
{
	if (test_spins_held)
		test_fatal("sleep with a spinlock held", "hrtimer_sleep_until");
	test_task->state = TASK_BLOCKED;
	hs_block(g_inst, task_still_blocked, abs_ns);
	test_task->state = TASK_RUNNING;
	if (remaining_ns)
		*remaining_ns = 0;
	return 0;
}

uint32_t percpu_get_online_count(void)
{
	return 1;
}

/* ------------------------------------------------------------------ */
/* softirq: a table and a deterministic pump in place of ksoftirqd      */
/* ------------------------------------------------------------------ */

static softirq_fn_t sirq_handlers[NR_SOFTIRQ];
static uint32_t sirq_pending;
uint64_t test_softirq_runs[NR_SOFTIRQ];

void softirq_register(uint32_t nr, softirq_fn_t fn)
{
	BUG_ON(fn == NULL);
	if (nr >= NR_SOFTIRQ)
		return;
	WARN_ON(sirq_handlers[nr] != NULL);
	sirq_handlers[nr] = fn;
}

void softirq_raise(uint32_t nr)
{
	if (nr < NR_SOFTIRQ)
		sirq_pending |= 1u << nr;
}

void softirq_raise_on(uint32_t cpu, uint32_t nr)
{
	(void)cpu;
	softirq_raise(nr);
}

/* Run every pending vector, lowest first, until none is pending, with
 * "interrupts" on -- what ksoftirqd does.  Not re-entered: a raise from a
 * handler is picked up by the loop. */
void softirq_drain(void)
{
	if (test_in_softirq)
		return;
	if (test_spins_held)
		test_fatalf("softirq run with %d spinlock(s) held",
			    test_spins_held);
	test_in_softirq = 1;
	for (int guard = 0; sirq_pending; guard++) {
		uint32_t mask = sirq_pending;

		if (guard > 1000000)
			test_fatal("softirq livelock (a vector re-raises "
				   "itself for ever)",
				   "softirq_drain");
		sirq_pending = 0;
		for (uint32_t i = 0; i < NR_SOFTIRQ; i++) {
			if (!(mask & (1u << i)) || !sirq_handlers[i])
				continue;
			test_softirq_runs[i]++;
			sirq_handlers[i]();
			if (test_spins_held || !test_irq_flag)
				test_fatalf("softirq vector %u returned with "
					    "%d spinlock(s) held, irq %s",
					    i, test_spins_held,
					    test_irq_flag ? "on" : "off");
		}
	}
	test_in_softirq = 0;
}

uint32_t softirq_pending_get(uint32_t cpu)
{
	(void)cpu;
	return sirq_pending;
}

int ksoftirqd_state_get(uint32_t cpu)
{
	(void)cpu;
	return -1;
}

void ksoftirqd_start_all(void)
{
}

/* ------------------------------------------------------------------ */
/* poll notification (kernel/net/poll.c's, without the poll layer)      */
/* ------------------------------------------------------------------ */

uint64_t test_poll_seq;

void poll_notify_wq(struct wait_queue_head *h)
{
	test_poll_seq++;
	if (h) {
		wq_wake_all(h);
		hs_notify(g_inst, glue_sock_index(h));
	}
}

void poll_notify_io_ready(void)
{
	test_poll_seq++;
}

void poll_notify_wq_key(struct wait_queue_head *h, unsigned key)
{
	(void)key;
	poll_notify_wq(h);
}

/* The descriptor layer's epoll (kernel/net/eventpoll.c, tested on its
 * own): nothing watches the harness's sockets. */
void eventpoll_release(void *entry)
{
	(void)entry;
}

/* Signals the socket layer raises on the caller (SIGPIPE): counted. */
uint64_t test_signals_raised;
int test_last_signal;

void sched_signal_task(task_t *task, int sig)
{
	(void)task;
	test_signals_raised++;
	test_last_signal = sig;
}

uint64_t timer_realtime_offset_ns(uint64_t mono)
{
	(void)mono;
	return 1767225600ULL * 1000000000ULL;
}

/* ------------------------------------------------------------------ */
/* user memory: the harness's "user" buffers are host memory            */
/* ------------------------------------------------------------------ */

void smap_disable(void)
{
}

void smap_enable(void)
{
}

bool validate_user_ptr(uint64_t ptr, size_t len)
{
	return ptr != 0 && ptr + len >= ptr;
}

int copy_from_user(void *kernel_dst, const void *user_src, size_t len)
{
	if (len && !validate_user_ptr((uint64_t)(uintptr_t)user_src, len))
		return -EFAULT;
	hs_memcpy(kernel_dst, user_src, len);
	return 0;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t len)
{
	if (len && !validate_user_ptr((uint64_t)(uintptr_t)user_dst, len))
		return -EFAULT;
	hs_memcpy(user_dst, kernel_src, len);
	return 0;
}

void mm_prefault_user_range(uint64_t addr, uint64_t len, int write)
{
	(void)addr;
	(void)len;
	(void)write;
}

/* ------------------------------------------------------------------ */
/* other subsystems                                                     */
/* ------------------------------------------------------------------ */

/* The NIC drivers: the instance's one device is the glue's simulated
 * NIC, registered after net_init(). */
void e1000_init(void)
{
}
void e1000e_init(void)
{
}
void rtl8139_init(void)
{
}
void pcnet32_init(void)
{
}
void ne2k_init(void)
{
}
void vmxnet3_init(void)
{
}
void eepro100_init(void)
{
}
void igb_init(void)
{
}
void tulip_init(void)
{
}

/* DHCP and DNS are not compiled in: the glue configures the address the
 * way a lease would. */
void dhcp_init(void)
{
}

int dhcp_configured(void)
{
	return 1;
}

int dhcp_discover(net_device_t *dev)
{
	(void)dev;
	return 0;
}

void dhcp_rx(net_device_t *dev, const uint8_t *data, uint16_t len)
{
	(void)dev;
	(void)data;
	(void)len;
}

void dhcp_tick(void)
{
}

void dhcp_invalidate(net_device_t *dev)
{
	(void)dev;
}

int dhcp_get_status(void)
{
	return 1;
}

int dhcp_release(net_device_t *dev)
{
	(void)dev;
	return 0;
}

int dhcp_renew(net_device_t *dev)
{
	(void)dev;
	return 0;
}

void dns_init(void)
{
}

void dns_rx(const uint8_t *data, uint16_t len)
{
	(void)data;
	(void)len;
}
