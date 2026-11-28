// LikeOS -- memory reclaim: watermarks, cache shrinkers and the background
// reclaim thread (see vmscan.h).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/mm/vmscan.h>
#include <kernel/mm/memory.h>
#include <kernel/fs/pagecache.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/mutex.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* ---- watermarks ------------------------------------------------------- */

/* The levels follow the machine.  The reserve at the bottom (min) grows
 * with the square root of memory -- 4 * sqrt(RAM in KB) KB, a few MB on a
 * small machine and a few tens on a big one, never below 1 MB or above
 * 64 MB -- and the band above it (to low, and as much again to high) is a
 * quarter of that or a thousandth of memory, whichever is more, so a big
 * machine starts giving memory back while there is still plenty left for a
 * burst of allocations.
 *
 * These used to be 2 MB and 8 MB whatever the machine had, which on a
 * machine with gigabytes meant reclaim started only when allocations were
 * already about to fail. */
static uint64_t g_wmark_min, g_wmark_low, g_wmark_high;

static uint64_t isqrt64(uint64_t v)
{
	uint64_t r = 0, bit = 1ULL << 62;

	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

static void vm_wmarks_compute(void)
{
	uint64_t ram_kb = mm_get_usable_pages() * (PAGE_SIZE / 1024);
	uint64_t min_kb = 4 * isqrt64(ram_kb);
	uint64_t band_kb;

	if (min_kb < 1024)
		min_kb = 1024;
	if (min_kb > 65536)
		min_kb = 65536;
	band_kb = min_kb / 4;
	if (ram_kb / 1000 > band_kb)
		band_kb = ram_kb / 1000;
	g_wmark_min = min_kb / (PAGE_SIZE / 1024);
	g_wmark_low = (min_kb + band_kb) / (PAGE_SIZE / 1024);
	/* Published last: a reader that sees `high' set sees the others. */
	__atomic_store_n(&g_wmark_high,
			 (min_kb + 2 * band_kb) / (PAGE_SIZE / 1024),
			 __ATOMIC_RELEASE);
}

static void vm_wmarks_ready(void)
{
	if (!__atomic_load_n(&g_wmark_high, __ATOMIC_ACQUIRE))
		vm_wmarks_compute();
}

uint64_t vm_wmark_min(void)
{
	vm_wmarks_ready();
	return g_wmark_min;
}

uint64_t vm_wmark_low(void)
{
	vm_wmarks_ready();
	return g_wmark_low;
}

uint64_t vm_wmark_high(void)
{
	vm_wmarks_ready();
	return g_wmark_high;
}

/* ---- shrinkers -------------------------------------------------------- */

/* One lock for the list and for every scan: a cache unregistering itself
 * waits for a scan of it to finish, after which nothing can call into it.
 * Scans sleep (they take the caches' own locks), hence a mutex. */
static DEFINE_MUTEX(g_shrinker_lock);
static struct vm_shrinker *g_shrinkers;

void vm_register_shrinker(struct vm_shrinker *s)
{
	if (WARN_ON(!s || !s->count || !s->scan))
		return;
	mutex_lock(&g_shrinker_lock);
	s->next = g_shrinkers;
	g_shrinkers = s;
	mutex_unlock(&g_shrinker_lock);
}

void vm_unregister_shrinker(struct vm_shrinker *s)
{
	struct vm_shrinker **pp;

	if (!s)
		return;
	mutex_lock(&g_shrinker_lock);
	for (pp = &g_shrinkers; *pp; pp = &(*pp)->next) {
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	}
	s->next = NULL;
	mutex_unlock(&g_shrinker_lock);
}

/* Ask every cache for its share: at priority p, 1/2^p of what it could
 * free.  The page cache is asked the same way below, so both come down
 * together and neither is emptied while the other keeps everything. */
static unsigned long vm_shrink_caches(int priority)
{
	unsigned long freed = 0;

	mutex_lock(&g_shrinker_lock);
	for (struct vm_shrinker *s = g_shrinkers; s; s = s->next) {
		unsigned long n = s->count(s->arg);
		unsigned long want = n >> priority;

		if (n && !want)
			want = 1;
		if (want)
			freed += s->scan(s->arg, want);
	}
	mutex_unlock(&g_shrinker_lock);
	return freed;
}

/* ---- the reclaim thread ----------------------------------------------- */

/* Batches of the page cache's own reclaim: it holds its LRU lock with
 * interrupts off for the length of one call, so one call stays short. */
#define VM_PC_BATCH 128

static unsigned long vm_shrink_pagecache(int priority)
{
	pc_stats_t st;
	unsigned long want, got = 0;

	pagecache_get_stats(&st);
	want = (unsigned long)(st.total_pages >> priority);
	if (want < 32)
		want = 32;
	while (want) {
		unsigned long n = want > VM_PC_BATCH ? VM_PC_BATCH : want;
		unsigned long r = pagecache_shrink(n, 0);

		got += r;
		want -= n;
		if (r == 0 || mm_get_free_pages() >= vm_wmark_high())
			break;
	}
	return got;
}

/* Bring free memory back above the high level, starting gently and asking
 * for more each round that falls short.  Stops when there is nothing left
 * to give; dirty file pages are then handed to writeback, after which they
 * can be dropped too. */
#define VM_PRIORITY_START 10

static void vm_reclaim(void)
{
	int priority = VM_PRIORITY_START;

	while (mm_get_free_pages() < vm_wmark_high() && priority >= 0) {
		unsigned long freed = vm_shrink_pagecache(priority);

		freed += vm_shrink_caches(priority);
		if (mm_get_free_pages() >= vm_wmark_high())
			break;
		if (!freed) {
			if (priority == 0)
				break;
			priority--;
		} else if (priority > 0) {
			priority--;
		}
	}
	if (mm_get_free_pages() < vm_wmark_high())
		pagecache_request_writeback();
}

static volatile int g_reclaim_wanted;
static volatile int g_reclaim_ready;
static task_t *g_reclaim_task;
static uint8_t g_reclaim_stack[16384] __attribute__((aligned(16)));

/* Every 100 ms it looks at free memory by itself as well: a wake asked for
 * with interrupts off cannot be delivered then, and the check costs one
 * comparison. */
static void vm_reclaim_thread(void *arg)
{
	(void)arg;
	g_reclaim_ready = 1;
	for (;;) {
		task_t *self = sched_current();

		if (g_reclaim_wanted || mm_get_free_pages() < vm_wmark_low()) {
			g_reclaim_wanted = 0;
			vm_reclaim();
			continue;
		}
		if (self) {
			self->wait_channel = (void *)&g_reclaim_wanted;
			self->wakeup_tick =
				timer_ticks() + (timer_get_frequency() / 10);
			task_set_blocked(self);
			sched_schedule();
			self->wakeup_tick = 0;
			self->wait_channel = NULL;
			if (self->state != TASK_RUNNING)
				self->state = TASK_RUNNING;
		}
	}
}

void vm_wake_reclaim(void)
{
	g_reclaim_wanted = 1;
	if (g_reclaim_ready && irqs_enabled())
		sched_wake_channel_once((void *)&g_reclaim_wanted, 1);
}

void vm_check_free_memory(void)
{
	if (mm_get_free_pages() < vm_wmark_low())
		vm_wake_reclaim();
}

void vm_reclaim_start(void)
{
	const char *nm = "kreclaimd";
	unsigned i = 0;

	if (g_reclaim_task)
		return;
	vm_wmarks_compute();
	g_reclaim_task = sched_add_task(vm_reclaim_thread, 0, g_reclaim_stack,
					sizeof(g_reclaim_stack));
	BUG_ON(g_reclaim_task == NULL);
	for (; nm[i] && i < sizeof(g_reclaim_task->comm) - 1; i++)
		g_reclaim_task->comm[i] = nm[i];
	g_reclaim_task->comm[i] = '\0';
	kprintf("vmscan: free-memory levels min %llu KB, low %llu KB, high %llu KB\n",
		(unsigned long long)(g_wmark_min * (PAGE_SIZE / 1024)),
		(unsigned long long)(g_wmark_low * (PAGE_SIZE / 1024)),
		(unsigned long long)(g_wmark_high * (PAGE_SIZE / 1024)));
}
