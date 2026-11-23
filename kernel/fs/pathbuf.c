// LikeOS -- path buffers (see pathbuf.h).
//
// A path buffer is one page: PATHBUF_SIZE is the page size, so a buffer is
// exactly one physical page reached through the direct map -- no page-table
// work to get it and no TLB shootdown to give it back.  A page outside the
// direct map (never the case on the machines the kernel runs on, but not
// impossible) is given back, and the buffer comes from kalloc() instead;
// the two are told apart on the way back by the address: a direct-map page
// is page aligned, a kalloc() block of this size never is.
//
// Freed buffers are kept on a short free list, so the page allocator is
// asked only when more paths are in flight at once than ever before (up to
// PATHBUF_CACHE_MAX kept).  A few are put there at boot, so the first path
// operations do not depend on the page allocator at all.
//
// Everything runs under one spinlock taken with interrupts off, and the page
// allocator below it takes only its own (a leaf): get and put never sleep and
// are safe in any context but NMI.  Only when no page is free and interrupts
// are on (no spinlock held) does a get first drop clean page-cache pages, as
// kalloc() does.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/pathbuf.h>
#include <kernel/fs/pagecache.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

_Static_assert(PATHBUF_SIZE == PAGE_SIZE,
	       "a path buffer is one page of the direct map");

/* Free buffers kept for reuse, at most. */
#define PATHBUF_CACHE_MAX 64
/* Free buffers put on the list at boot. */
#define PATHBUF_RESERVE 16

/* A free buffer's first bytes. */
struct pathbuf_free {
	struct pathbuf_free *next;
	uint64_t magic;
};

#define PATHBUF_FREE_MAGIC 0x50415448465245ULL /* "PATHFRE" */

static spinlock_t g_pb_lock = SPINLOCK_INIT("pathbuf");
static struct pathbuf_free *g_pb_free;
static unsigned g_pb_nfree;
static unsigned long g_pb_in_use;

/* A new buffer from the page allocator (or kalloc), or NULL. */
static char *pathbuf_alloc(void)
{
	uint64_t phys = mm_allocate_physical_page();

	if (!phys && irqs_enabled()) {
		/* No free page.  As the slab allocator does: with interrupts
		 * on the caller holds no spinlock, so clean page-cache pages
		 * can be dropped and the allocation tried once more.  Under a
		 * spinlock nothing can be done here, and the caller gets
		 * ENOMEM. */
		pagecache_shrink(32, 0);
		pagecache_request_writeback();
		phys = mm_allocate_physical_page();
	}
	if (phys) {
		if (is_phys_in_direct_map(phys + PAGE_SIZE - 1))
			return (char *)phys_to_virt(phys);
		mm_free_physical_page(phys);
	}
	return (char *)kalloc(PATHBUF_SIZE);
}

/* Give a buffer back to where pathbuf_alloc() got it. */
static void pathbuf_release(char *buf)
{
	uint64_t a = (uint64_t)(uintptr_t)buf;

	if (!(a & (PAGE_SIZE - 1)) && is_direct_map_addr(a))
		mm_free_physical_page(virt_to_phys(buf));
	else
		kfree(buf);
}

char *pathbuf_get(void)
{
	struct pathbuf_free *f;
	uint64_t fl;
	char *buf;

	spin_lock_irqsave(&g_pb_lock, &fl);
	f = g_pb_free;
	if (f) {
		WARN_ON(f->magic != PATHBUF_FREE_MAGIC);
		g_pb_free = f->next;
		g_pb_nfree--;
		f->magic = 0;
	}
	g_pb_in_use++;
	spin_unlock_irqrestore(&g_pb_lock, fl);
	if (f)
		return (char *)f;

	buf = pathbuf_alloc();
	if (!buf) {
		spin_lock_irqsave(&g_pb_lock, &fl);
		g_pb_in_use--;
		spin_unlock_irqrestore(&g_pb_lock, fl);
	}
	return buf;
}

void pathbuf_put(char *buf)
{
	struct pathbuf_free *f = (struct pathbuf_free *)buf;
	uint64_t fl;

	if (!buf)
		return;
#if DEBUG
	/* Freed memory is poisoned so that a use after the put shows. */
	for (unsigned long i = sizeof(*f); i < PATHBUF_SIZE; i++)
		buf[i] = (char)0x6b;
#endif
	spin_lock_irqsave(&g_pb_lock, &fl);
#if DEBUG
	for (struct pathbuf_free *p = g_pb_free; p; p = p->next)
		BUG_ON(p == f); /* put twice */
#endif
	WARN_ON(g_pb_in_use == 0);
	if (g_pb_in_use)
		g_pb_in_use--;
	if (g_pb_nfree < PATHBUF_CACHE_MAX) {
		f->magic = PATHBUF_FREE_MAGIC;
		f->next = g_pb_free;
		g_pb_free = f;
		g_pb_nfree++;
		f = NULL;
	}
	spin_unlock_irqrestore(&g_pb_lock, fl);
	if (f)
		pathbuf_release(buf);
}

unsigned long pathbuf_in_use(void)
{
	return __atomic_load_n(&g_pb_in_use, __ATOMIC_RELAXED);
}

void pathbuf_init(void)
{
	for (int i = 0; i < PATHBUF_RESERVE; i++) {
		struct pathbuf_free *f = (struct pathbuf_free *)pathbuf_alloc();
		uint64_t fl;

		if (!f) {
			kprintf("pathbuf: reserve only %d of %d buffers\n", i,
				PATHBUF_RESERVE);
			return;
		}
		spin_lock_irqsave(&g_pb_lock, &fl);
		if (g_pb_nfree >= PATHBUF_CACHE_MAX) {
			spin_unlock_irqrestore(&g_pb_lock, fl);
			pathbuf_release((char *)f);
			return;
		}
		f->magic = PATHBUF_FREE_MAGIC;
		f->next = g_pb_free;
		g_pb_free = f;
		g_pb_nfree++;
		spin_unlock_irqrestore(&g_pb_lock, fl);
	}
}
