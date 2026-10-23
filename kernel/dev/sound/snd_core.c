// LikeOS sound core - cards, device nodes, events, deferred work, DMA
// memory and the entry points.
//
// A sound card driver creates a card (snd_card_new), adds its PCM devices
// (snd_pcm_new, snd_pcm.c) and its control operations, and registers it.
// This file then publishes the card under /dev:
//
//   card 0:   /dev/audio0 (14,4)   /dev/dsp (14,3)     /dev/mixer (14,0)
//   card n:   /dev/audioN          /dev/dspN           /dev/mixerN
//             minor = (n << 4) | function
//
// all crw-rw---- root:audio, and dispatches the file operations of those
// nodes to the PCM layer (snd_pcm.c), the control layer (snd_ctl.c) and the
// OSS translation (snd_oss.c).
//
// It also owns the pieces every driver shares: the per-card event queue
// behind AUDIO_GETEVENT and POLLPRI, the "ksnd" kernel thread that runs
// deferred work (codec verbs after an unsolicited response, firmware loads,
// jack polling), physically contiguous DMA memory for driver structures,
// and the hookup between the display driver and the HDMI codec code.
//
// Copyright (C) 2026 The LikeOS Project

#include "snd_internal.h"
#include "hda.h"

#include <kernel/uapi/ioctl.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/hal/lapic.h>
#include <kernel/fs/sysfs.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>

/* Not exported by a header: the physical address of a block in the
 * low kernel heap (kalloc_dma). */
uint64_t mm_virt_to_phys_heap(uint64_t virt);

/* ======================================================================
 * Small helpers
 * ====================================================================== */

void snd_strlcpy(char *dst, const char *src, size_t cap)
{
	size_t i = 0;

	if (!dst || cap == 0)
		return;
	if (src) {
		while (src[i] && i < cap - 1) {
			dst[i] = src[i];
			i++;
		}
	}
	/* Pad the rest so a structure copied to user space never carries
	 * whatever the buffer held before. */
	while (i < cap)
		dst[i++] = 0;
}

int snd_user_in(void *dst, const void *user, size_t n)
{
	if (n == 0)
		return 0;
	if (!user || !validate_user_ptr((uint64_t)user, n))
		return -EFAULT;
	return copy_from_user(dst, user, n) == 0 ? 0 : -EFAULT;
}

int snd_user_out(void *user, const void *src, size_t n)
{
	if (n == 0)
		return 0;
	if (!user || !validate_user_ptr((uint64_t)user, n))
		return -EFAULT;
	return copy_to_user(user, src, n) == 0 ? 0 : -EFAULT;
}

void snd_text_printf(struct snd_text *t, const char *fmt, ...)
{
	__builtin_va_list ap;

	if (!t || !t->buf || t->cap == 0 || t->len + 1 >= t->cap)
		return;
	__builtin_va_start(ap, fmt);
	kvsnprintf(t->buf + t->len, t->cap - t->len, fmt, ap);
	__builtin_va_end(ap);
	/* The formatter's return value is not a reliable "bytes stored"
	 * on truncation; measure instead. */
	t->len += kstrlen(t->buf + t->len);
	if (t->len >= t->cap)
		t->len = t->cap - 1;
}

/* ======================================================================
 * Sleeping lock and waits
 * ====================================================================== */

void snd_mutex_init(struct snd_mutex *m, const char *name)
{
	spinlock_init(&m->lock, name);
	m->locked = 0;
	m->owner = NULL;
	wq_head_init(&m->wq, name);
}

static int snd_mutex_free_cond(void *arg)
{
	struct snd_mutex *m = arg;
	return !__atomic_load_n(&m->locked, __ATOMIC_ACQUIRE);
}

int snd_mutex_lock(struct snd_mutex *m, int interruptible)
{
	task_t *cur = sched_current();

	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&m->lock, &fl);
		if (!m->locked) {
			m->locked = 1;
			m->owner = cur;
			spin_unlock_irqrestore(&m->lock, fl);
			return 0;
		}
		spin_unlock_irqrestore(&m->lock, fl);
		int rc = snd_wait_event(&m->wq, snd_mutex_free_cond, m, 0,
					interruptible);
		if (rc == -ERESTARTSYS)
			return rc;
	}
}

void snd_mutex_unlock(struct snd_mutex *m)
{
	uint64_t fl;

	spin_lock_irqsave(&m->lock, &fl);
	m->locked = 0;
	m->owner = NULL;
	spin_unlock_irqrestore(&m->lock, fl);
	wq_wake_all(&m->wq);
}

int snd_wait_event(struct wait_queue_head *wq, snd_cond_fn cond, void *arg,
		   uint64_t timeout_ns, int interruptible)
{
	task_t *cur = sched_current();
	uint64_t deadline = 0;

	if (timeout_ns) {
		uint64_t now = hrtimer_now_ns();
		deadline = timeout_ns > ~0ULL - now ? ~0ULL : now + timeout_ns;
	}

	for (;;) {
		if (cond(arg))
			return 0;
		if (!cur) {
			/* No scheduler yet (early boot): nothing else can run,
			 * so the only thing that changes the condition is an
			 * interrupt.  Spin politely, bounded by the timeout. */
			if (deadline && hrtimer_now_ns() >= deadline)
				return -ETIMEDOUT;
			__asm__ volatile("pause");
			continue;
		}
		if (interruptible && signal_pending(cur))
			return -ERESTARTSYS;
		if (deadline && hrtimer_now_ns() >= deadline)
			return -ETIMEDOUT;

		struct wait_queue_entry we;
		uint64_t fl = local_irq_save();
		wq_entry_init(&we, cur);
		wq_add(wq, &we);
		cur->wait_channel = wq;
		if (deadline) {
			uint64_t left = deadline - hrtimer_now_ns();
			if ((int64_t)left < 0)
				left = 0;
			/* The tick deadline is the backstop; the precise end
			 * comes from the high-resolution timer. */
			cur->wakeup_tick = timer_ticks() + timer_ns_to_ticks(left) + 1;
		}
		/* Blocked first, the condition looked at second: a wake from
		 * another processor between the look and the state store
		 * would otherwise find the task running, do nothing, and leave
		 * it asleep with the condition true.  Undone with a
		 * compare-and-swap, since a waker may already have claimed the
		 * task. */
		task_set_blocked(cur);
		if (deadline)
			hrtimer_wait_arm(cur, deadline);
		if (cond(arg)) {
			task_state_t expected = TASK_BLOCKED;
			if (__atomic_compare_exchange_n(&cur->state, &expected,
							TASK_RUNNING, false,
							__ATOMIC_ACQ_REL,
							__ATOMIC_ACQUIRE)) {
				if (deadline)
					hrtimer_wait_disarm(cur);
				cur->wakeup_tick = 0;
				cur->wait_channel = NULL;
				local_irq_restore(fl);
				wq_remove(wq, &we);
				return 0;
			}
		}
		local_irq_restore(fl);
		sched_schedule();
		if (deadline)
			hrtimer_wait_disarm(cur);
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		wq_remove(wq, &we);
	}
}

/* ======================================================================
 * DMA memory
 * ====================================================================== */

struct snd_dma_mem {
	uint64_t phys;
	uint8_t *virt;
	uint32_t pages;
	void *heap;
	int uc_mapped;
};

#define SND_DMA_MAX_PAGES (64u * 1024u * 1024u / PAGE_SIZE) /* 64 MiB */
#define SND_DMA_HEAP_MAX (256u * 1024u) /* kalloc_dma fallback limit */
#define SND_4G 0x100000000ULL

static void snd_clflush_range(const void *p, size_t bytes)
{
	const uint8_t *b = (const uint8_t *)((uint64_t)p & ~63ULL);
	const uint8_t *e = (const uint8_t *)p + bytes;

	for (; b < e; b += 64)
		__asm__ volatile("clflush (%0)" ::"r"(b) : "memory");
	__asm__ volatile("mfence" ::: "memory");
}

static size_t snd_pow2_ceil(size_t v)
{
	size_t p = 1;
	while (p < v && p < ((size_t)1 << 30))
		p <<= 1;
	return p;
}

/* Contiguous, zeroed, optionally uncached memory.  The cache handling:
 * the pages are zeroed through their ordinary write-back alias, every line
 * is flushed out, and only then is the kernel's view switched to uncached,
 * so no dirty line can later be written back over what the device put
 * there. */
static int snd_dma_mem_alloc(size_t bytes, size_t align_bytes, int dma32,
			     int coherent, struct snd_dma_mem *out)
{
	uint64_t pages64 = ((uint64_t)bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	size_t align_pages;
	uint64_t phys = 0;
	uint8_t *wb = NULL;
	void *heap = NULL;

	mm_memset(out, 0, sizeof(*out));
	if (bytes == 0 || pages64 == 0 || pages64 > SND_DMA_MAX_PAGES)
		return -EINVAL;
	align_pages = align_bytes > PAGE_SIZE ?
			      snd_pow2_ceil((align_bytes + PAGE_SIZE - 1) / PAGE_SIZE) :
			      1;
	if (align_pages > 1024)
		return -EINVAL;

	phys = mm_allocate_contiguous_pages_aligned((size_t)pages64, align_pages);
	if (phys && dma32 && phys + pages64 * PAGE_SIZE > SND_4G) {
		/* The allocator prefers low memory, so this only happens when
		 * everything below 4 GiB is fragmented or taken. */
		mm_free_contiguous_pages(phys, (size_t)pages64);
		phys = 0;
	}
	if (phys) {
		if (is_phys_in_direct_map(phys + pages64 * PAGE_SIZE - 1))
			wb = phys_to_virt(phys);
	} else if (dma32 && pages64 * PAGE_SIZE <= SND_DMA_HEAP_MAX) {
		/* Last resort for a 32-bit device: the low kernel heap.  The
		 * block is over-allocated so that whole pages inside it are
		 * ours alone (the ring may be mapped into a process, which
		 * must never see a neighbouring kernel object). */
		size_t slack = align_pages * PAGE_SIZE;
		heap = kalloc_dma((size_t)pages64 * PAGE_SIZE + slack);
		if (!heap)
			return -ENOMEM;
		uint64_t v = ((uint64_t)heap + slack - 1) & ~((uint64_t)slack - 1);
		if (v < (uint64_t)heap)
			v = (uint64_t)heap;
		phys = mm_virt_to_phys_heap(v);
		if (!phys || phys + pages64 * PAGE_SIZE > SND_4G ||
		    (phys & (PAGE_SIZE - 1))) {
			kfree_dma(heap);
			return -ENOMEM;
		}
		wb = (uint8_t *)v;
	} else {
		return -ENOMEM;
	}

	out->phys = phys;
	out->pages = (uint32_t)pages64;
	out->heap = heap;

	if (wb) {
		mm_memset(wb, 0, (size_t)pages64 * PAGE_SIZE);
		if (coherent) {
			out->virt = wb;
			return 0;
		}
		snd_clflush_range(wb, (size_t)pages64 * PAGE_SIZE);
	}
	uint64_t va = mm_map_mmio_flags(phys, (size_t)pages64, MM_MMIO_UC);
	if (!va) {
		if (heap)
			kfree_dma(heap);
		else
			mm_free_contiguous_pages(phys, (size_t)pages64);
		mm_memset(out, 0, sizeof(*out));
		return -ENOMEM;
	}
	out->virt = (uint8_t *)va;
	out->uc_mapped = 1;
	if (!wb)
		mm_memset(out->virt, 0, (size_t)pages64 * PAGE_SIZE);
	return 0;
}

static void snd_dma_mem_free(struct snd_dma_mem *m)
{
	if (!m || !m->pages)
		return;
	if (m->uc_mapped)
		mm_unmap_mmio((uint64_t)m->virt, m->pages);
	if (m->heap)
		kfree_dma(m->heap);
	else
		mm_free_contiguous_pages(m->phys, m->pages);
	mm_memset(m, 0, sizeof(*m));
}

/* Driver allocations are remembered by their kernel address so
 * snd_dma_free() can undo exactly what was done. */
struct snd_dma_rec {
	struct snd_dma_rec *next;
	struct snd_dma_mem mem;
	size_t bytes;
};

static struct snd_dma_rec *g_dma_recs;
static spinlock_t g_dma_lock = SPINLOCK_INIT("snd_dma");

void *snd_dma_alloc(size_t bytes, int dma32, int coherent, uint64_t *phys)
{
	struct snd_dma_rec *r;
	uint64_t fl;

	if (phys)
		*phys = 0;
	if (bytes == 0)
		return NULL;
	r = kalloc(sizeof(*r));
	if (!r)
		return NULL;
	mm_memset(r, 0, sizeof(*r));
	/* 128-byte alignment covers every controller structure (BDL, CORB,
	 * RIRB, position buffer); the allocation is page-aligned anyway. */
	if (snd_dma_mem_alloc(bytes, PAGE_SIZE, dma32, coherent, &r->mem) != 0) {
		kfree(r);
		kprintf("[snd] core: no DMA memory for %lu bytes%s\n",
			(unsigned long)bytes, dma32 ? " below 4 GiB" : "");
		return NULL;
	}
	r->bytes = bytes;
	spin_lock_irqsave(&g_dma_lock, &fl);
	r->next = g_dma_recs;
	g_dma_recs = r;
	spin_unlock_irqrestore(&g_dma_lock, fl);
	if (phys)
		*phys = r->mem.phys;
	return r->mem.virt;
}

void snd_dma_free(void *virt, size_t bytes)
{
	struct snd_dma_rec **pp, *r = NULL;
	uint64_t fl;

	if (!virt)
		return;
	spin_lock_irqsave(&g_dma_lock, &fl);
	for (pp = &g_dma_recs; *pp; pp = &(*pp)->next) {
		if ((void *)(*pp)->mem.virt == virt) {
			r = *pp;
			*pp = r->next;
			break;
		}
	}
	spin_unlock_irqrestore(&g_dma_lock, fl);
	if (!r) {
		kprintf("[snd] core: snd_dma_free of unknown block %p\n", virt);
		return;
	}
	if (bytes && bytes != r->bytes)
		kprintf("[snd] core: snd_dma_free size mismatch (%lu, allocated %lu)\n",
			(unsigned long)bytes, (unsigned long)r->bytes);
	snd_dma_mem_free(&r->mem);
	kfree(r);
}

/* ---- PCM rings --------------------------------------------------------- */

struct snd_ring *snd_ring_alloc(uint32_t bytes, uint32_t align, int dma32,
				int coherent)
{
	struct snd_ring *r;
	struct snd_dma_mem m;

	if (bytes == 0 || bytes > SND_RING_MAX_BYTES)
		return NULL;
	r = kalloc(sizeof(*r));
	if (!r)
		return NULL;
	mm_memset(r, 0, sizeof(*r));
	if (snd_dma_mem_alloc(bytes, align, dma32, coherent, &m) != 0) {
		kfree(r);
		return NULL;
	}
	r->refs = 1;
	r->phys = m.phys;
	r->virt = m.virt;
	r->pages = m.pages;
	r->bytes = bytes;
	r->dma32 = dma32;
	r->coherent = coherent;
	r->uc_mapped = m.uc_mapped;
	r->heap = m.heap;
	return r;
}

void snd_ring_get(struct snd_ring *r)
{
	if (r)
		__atomic_fetch_add(&r->refs, 1, __ATOMIC_ACQ_REL);
}

static void snd_ring_destroy(struct snd_ring *r)
{
	struct snd_dma_mem m;

	m.phys = r->phys;
	m.virt = r->virt;
	m.pages = r->pages;
	m.heap = r->heap;
	m.uc_mapped = r->uc_mapped;
	snd_dma_mem_free(&m);
	mm_memset(r, 0, sizeof(*r));
	kfree(r);
}

void snd_ring_put(struct snd_ring *r)
{
	if (!r)
		return;
	int left = __atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL);
	if (left == 0)
		snd_ring_destroy(r);
	else if (left < 0)
		kprintf("[snd] core: ring reference count underflow\n");
}

static task_t *g_ksnd;

static void snd_ring_destroy_work(void *arg)
{
	snd_ring_destroy(arg);
}

void snd_ring_put_async(struct snd_ring *r)
{
	if (!r)
		return;
	int left = __atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL);
	if (left > 0)
		return;
	if (left < 0) {
		kprintf("[snd] core: ring reference count underflow\n");
		return;
	}
	/* Restoring the kernel's cache attribute and freeing the pages
	 * shoots down TLBs; the address space calls in here with its own
	 * locks held, so the work moves to ksnd when it exists. */
	if (!g_ksnd || snd_schedule_work(snd_ring_destroy_work, r, 0) != 0)
		snd_ring_destroy(r);
}

/* ======================================================================
 * Deferred work: the ksnd thread
 * ====================================================================== */

#define SND_WORK_FREE 0
#define SND_WORK_DELAYED 1
#define SND_WORK_READY 2

struct snd_work {
	snd_work_fn fn;
	void *arg;
	uint64_t due_ns;
	uint64_t seq;
	int state;
};

static struct snd_work g_work[SND_WORK_SLOTS];
static spinlock_t g_work_lock = SPINLOCK_INIT("snd_work");
static struct wait_queue_head g_work_wq;
static hrtimer_t g_work_timer;
static uint64_t g_work_seq;
static int g_work_inited;
static int g_work_full_reported;
static uint8_t g_ksnd_stack[32768] __attribute__((aligned(16)));

static void snd_work_timer_fn(hrtimer_t *t);

static void snd_work_init_once(void)
{
	uint64_t fl;

	spin_lock_irqsave(&g_work_lock, &fl);
	if (!g_work_inited) {
		wq_head_init(&g_work_wq, "ksnd");
		hrtimer_init(&g_work_timer, snd_work_timer_fn, NULL);
		g_work_inited = 1;
	}
	spin_unlock_irqrestore(&g_work_lock, fl);
}

/* With g_work_lock held: arm the timer for the earliest delayed item. */
static void snd_work_rearm_locked(void)
{
	uint64_t next = ~0ULL;

	for (int i = 0; i < SND_WORK_SLOTS; i++)
		if (g_work[i].state == SND_WORK_DELAYED && g_work[i].due_ns < next)
			next = g_work[i].due_ns;
	if (next != ~0ULL)
		hrtimer_start(&g_work_timer, next);
}

static void snd_work_timer_fn(hrtimer_t *t)
{
	uint64_t fl, now = hrtimer_now_ns();
	int ready = 0;
	(void)t;

	spin_lock_irqsave(&g_work_lock, &fl);
	for (int i = 0; i < SND_WORK_SLOTS; i++) {
		if (g_work[i].state == SND_WORK_DELAYED && g_work[i].due_ns <= now) {
			g_work[i].state = SND_WORK_READY;
			ready = 1;
		}
	}
	snd_work_rearm_locked();
	spin_unlock_irqrestore(&g_work_lock, fl);
	if (ready)
		wq_wake_all(&g_work_wq);
}

int snd_schedule_work(snd_work_fn fn, void *arg, uint32_t delay_ms)
{
	uint64_t fl;
	int slot = -1;

	if (!fn)
		return -EINVAL;
	if (!g_work_inited)
		snd_work_init_once();
	spin_lock_irqsave(&g_work_lock, &fl);
	for (int i = 0; i < SND_WORK_SLOTS; i++) {
		if (g_work[i].state == SND_WORK_FREE) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		int report = !g_work_full_reported;
		g_work_full_reported = 1;
		spin_unlock_irqrestore(&g_work_lock, fl);
		if (report)
			kprintf("[snd] core: deferred work queue full (%d items); dropping\n",
				SND_WORK_SLOTS);
		return -ENOSPC;
	}
	struct snd_work *w = &g_work[slot];
	w->fn = fn;
	w->arg = arg;
	w->seq = ++g_work_seq;
	if (delay_ms) {
		w->due_ns = hrtimer_now_ns() + (uint64_t)delay_ms * 1000000ULL;
		w->state = SND_WORK_DELAYED;
		snd_work_rearm_locked();
	} else {
		w->due_ns = 0;
		w->state = SND_WORK_READY;
	}
	spin_unlock_irqrestore(&g_work_lock, fl);
	if (!delay_ms)
		wq_wake_all(&g_work_wq);
	return 0;
}

static int snd_work_ready_cond(void *arg)
{
	uint64_t fl, now = hrtimer_now_ns();
	int ready = 0;
	(void)arg;

	spin_lock_irqsave(&g_work_lock, &fl);
	for (int i = 0; i < SND_WORK_SLOTS; i++) {
		/* A delayed item whose time has come counts too: the timer
		 * may not have run yet (tick-driven fallback). */
		if (g_work[i].state == SND_WORK_DELAYED && g_work[i].due_ns <= now)
			g_work[i].state = SND_WORK_READY;
		if (g_work[i].state == SND_WORK_READY)
			ready = 1;
	}
	spin_unlock_irqrestore(&g_work_lock, fl);
	return ready;
}

/* Take the oldest ready item (submission order). */
static int snd_work_take(snd_work_fn *fn, void **arg)
{
	uint64_t fl;
	int best = -1;

	spin_lock_irqsave(&g_work_lock, &fl);
	for (int i = 0; i < SND_WORK_SLOTS; i++) {
		if (g_work[i].state != SND_WORK_READY)
			continue;
		if (best < 0 || g_work[i].seq < g_work[best].seq)
			best = i;
	}
	if (best >= 0) {
		*fn = g_work[best].fn;
		*arg = g_work[best].arg;
		g_work[best].state = SND_WORK_FREE;
		g_work[best].fn = NULL;
		g_work[best].arg = NULL;
		g_work_full_reported = 0;
	}
	spin_unlock_irqrestore(&g_work_lock, fl);
	return best >= 0;
}

static void ksnd_main(void *unused)
{
	(void)unused;
	for (;;) {
		snd_work_fn fn;
		void *arg;

		while (snd_work_take(&fn, &arg))
			fn(arg);
		(void)snd_wait_event(&g_work_wq, snd_work_ready_cond, NULL, 0, 0);
	}
}

/* ======================================================================
 * Delays
 * ====================================================================== */

void snd_udelay(uint32_t us)
{
	if (us)
		lapic_delay_us(us);
}

void snd_mdelay(uint32_t ms)
{
	while (ms) {
		uint32_t chunk = ms > 100 ? 100 : ms;
		lapic_delay_ms(chunk);
		ms -= chunk;
	}
}

/* ======================================================================
 * Cards
 * ====================================================================== */

static struct snd_card *g_cards[SND_MAX_CARDS];
static spinlock_t g_cards_lock = SPINLOCK_INIT("snd_cards");

struct snd_card *snd_card_get(int index)
{
	struct snd_card *c = NULL;
	uint64_t fl;

	if (index < 0 || index >= SND_MAX_CARDS)
		return NULL;
	spin_lock_irqsave(&g_cards_lock, &fl);
	c = g_cards[index];
	spin_unlock_irqrestore(&g_cards_lock, fl);
	return c;
}

/* One past the highest registered card index (snd_card_get() answers NULL
 * for a slot whose card was freed). */
int snd_card_count(void)
{
	int n = 0;
	uint64_t fl;

	spin_lock_irqsave(&g_cards_lock, &fl);
	for (int i = 0; i < SND_MAX_CARDS; i++)
		if (g_cards[i])
			n = i + 1;
	spin_unlock_irqrestore(&g_cards_lock, fl);
	return n;
}

int snd_card_new(const char *driver, const pci_device_t *pci,
		 struct snd_card **out)
{
	struct snd_card_priv *cp;
	struct snd_card *card;

	if (!out)
		return -EINVAL;
	*out = NULL;
	cp = kalloc(sizeof(*cp));
	if (!cp)
		return -ENOMEM;
	mm_memset(cp, 0, sizeof(*cp));
	card = &cp->card;
	cp->magic = SND_CARD_MAGIC;
	cp->refs = 1; /* the driver's */
	cp->slot = -1;
	snd_mutex_init(&cp->ctl_mtx, "snd_ctl");
	card->index = -1;
	snd_strlcpy(card->driver, driver ? driver : "snd", sizeof(card->driver));
	snd_strlcpy(card->name, card->driver, sizeof(card->name));
	card->pci = pci;
	if (pci)
		ksnprintf(card->bus, sizeof(card->bus), "pci:0000:%02x:%02x.%x",
			  pci->bus, pci->device, pci->function);
	else
		ksnprintf(card->bus, sizeof(card->bus), "platform:%s", card->driver);
	spinlock_init(&card->ev_lock, "snd_ev");
	wq_head_init(&card->ev_wq, "snd_ev");
	*out = card;
	return 0;
}

static void snd_pcm_free_one(struct snd_pcm *pcm)
{
	struct snd_pcm_priv *pp = snd_pcm_priv(pcm);

	for (int d = 0; d < 2; d++) {
		struct snd_ss_rt *rt = &pp->rt[d];
		hrtimer_cancel(&rt->timer);
		/* hrtimer_cancel does not wait for a callback already running
		 * on another processor; this does. */
		while (__atomic_load_n(&rt->timer_cb_active, __ATOMIC_ACQUIRE))
			__asm__ volatile("pause");
		if (rt->ring) {
			struct snd_ring *r = rt->ring;
			rt->ring = NULL;
			pcm->sub[d].dma_area = NULL;
			pcm->sub[d].dma_phys = 0;
			snd_ring_put(r);
		}
	}
	mm_memset(pp, 0, sizeof(*pp));
	kfree(pp);
}

static void snd_card_destroy(struct snd_card_priv *cp)
{
	struct snd_card *card = &cp->card;

	for (int i = 0; i < card->npcm && i < SND_MAX_PCMS; i++) {
		if (card->pcm[i]) {
			snd_pcm_free_one(card->pcm[i]);
			card->pcm[i] = NULL;
		}
	}
	cp->magic = 0;
	kfree(cp);
}

void snd_card_ref(struct snd_card *card)
{
	if (card)
		__atomic_fetch_add(&snd_card_priv(card)->refs, 1, __ATOMIC_ACQ_REL);
}

void snd_card_unref(struct snd_card *card)
{
	if (!card)
		return;
	struct snd_card_priv *cp = snd_card_priv(card);
	if (__atomic_sub_fetch(&cp->refs, 1, __ATOMIC_ACQ_REL) == 0)
		snd_card_destroy(cp);
}

static void snd_card_unregister_nodes(struct snd_card *card)
{
	struct snd_card_priv *cp = snd_card_priv(card);
	struct devfs_node *nodes[3] = { &card->node_mixer, &card->node_dsp,
					&card->node_audio };

	for (int i = 0; i < 3; i++) {
		if (cp->node_ok[i]) {
			device_unregister(nodes[i]);
			cp->node_ok[i] = 0;
		}
	}
}

void snd_card_free(struct snd_card *card)
{
	struct snd_card_priv *cp;
	uint64_t fl;

	if (!card)
		return;
	cp = snd_card_priv(card);
	if (cp->magic != SND_CARD_MAGIC) {
		kprintf("[snd] core: snd_card_free of a bad card %p\n", card);
		return;
	}
	if (card->registered && !card->disconnected)
		snd_card_disconnect(card);
	snd_card_unregister_nodes(card);
	spin_lock_irqsave(&g_cards_lock, &fl);
	if (cp->slot >= 0 && cp->slot < SND_MAX_CARDS && g_cards[cp->slot] == card)
		g_cards[cp->slot] = NULL;
	spin_unlock_irqrestore(&g_cards_lock, fl);
	card->registered = 0;
	if (__atomic_load_n(&cp->open_files, __ATOMIC_ACQUIRE))
		kprintf("[snd] card%d: freed while %d handle(s) are open; memory kept until they close\n",
			card->index, cp->open_files);
	snd_card_unref(card);
}

/* ======================================================================
 * Events
 * ====================================================================== */

void snd_card_event(struct snd_card *card, uint32_t type, uint32_t id,
		    uint32_t value)
{
	uint64_t fl;

	if (!card)
		return;
	spin_lock_irqsave(&card->ev_lock, &fl);
	struct audio_event *e = &card->ev[card->ev_head % SND_EVENT_QUEUE];
	e->type = type;
	e->id = id;
	e->value = value;
	e->reserved = 0;
	e->timestamp_ns = hrtimer_now_ns();
	card->ev_head++;
	/* ev_tail: the oldest sequence number still held. */
	if (card->ev_head - card->ev_tail > SND_EVENT_QUEUE)
		card->ev_tail = card->ev_head - SND_EVENT_QUEUE;
	if (type == AUDIO_EV_VOLUME || type == AUDIO_EV_OUTPUT)
		card->modify_counter++;
	spin_unlock_irqrestore(&card->ev_lock, fl);
	poll_notify_wq(&card->ev_wq);
}

void snd_jack_report(struct snd_card *card, uint32_t output_id, int present)
{
	if (!card)
		return;
	snd_card_event(card, AUDIO_EV_JACK, output_id, present ? 1 : 0);
}

/* ======================================================================
 * Device node operations
 * ====================================================================== */

static struct snd_file *snd_file_of(vfs_file_t *file)
{
	struct snd_file *f = device_file_priv(file);
	if (!f || f->magic != SND_FILE_MAGIC)
		return NULL;
	return f;
}

/* The PCM device a handle uses: the first one that has every direction
 * asked for. */
static struct snd_pcm *snd_pick_pcm(struct snd_card *card, uint32_t want)
{
	for (int i = 0; i < card->npcm && i < SND_MAX_PCMS; i++) {
		struct snd_pcm *p = card->pcm[i];
		uint32_t have = 0;
		if (!p)
			continue;
		if (p->hw[AUDIO_DIR_PLAYBACK].formats)
			have |= 1u << AUDIO_DIR_PLAYBACK;
		if (p->hw[AUDIO_DIR_CAPTURE].formats)
			have |= 1u << AUDIO_DIR_CAPTURE;
		if ((have & want) == want)
			return p;
	}
	return NULL;
}

/* Directions a PCM device has. */
static uint32_t snd_pcm_dirs(const struct snd_pcm *p)
{
	uint32_t have = 0;
	if (p->hw[AUDIO_DIR_PLAYBACK].formats)
		have |= 1u << AUDIO_DIR_PLAYBACK;
	if (p->hw[AUDIO_DIR_CAPTURE].formats)
		have |= 1u << AUDIO_DIR_CAPTURE;
	return have;
}

/* Count a call into the handle; 0 while AUDIO_SELECTPCM swaps its PCM. */
static int snd_file_enter(struct snd_file *f)
{
	__atomic_fetch_add(&f->active, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&f->switching, __ATOMIC_SEQ_CST)) {
		__atomic_fetch_sub(&f->active, 1, __ATOMIC_SEQ_CST);
		return 0;
	}
	return 1;
}

static void snd_file_leave(struct snd_file *f)
{
	__atomic_fetch_sub(&f->active, 1, __ATOMIC_SEQ_CST);
}

static long snd_pcm_info_ioctl(struct snd_file *f, void *argp)
{
	struct snd_card *card = f->card;
	struct audio_pcm_info pi;
	struct snd_pcm *p;
	int n = card->npcm < SND_MAX_PCMS ? card->npcm : SND_MAX_PCMS;

	if (snd_user_in(&pi, argp, sizeof(pi)))
		return -EFAULT;
	if (pi.index >= (uint32_t)n || !(p = card->pcm[pi.index]))
		return -EINVAL;
	{
		uint32_t index = pi.index;
		mm_memset(&pi, 0, sizeof(pi));
		pi.index = index;
	}
	pi.count = (uint32_t)n;
	if (p->hw[AUDIO_DIR_PLAYBACK].formats)
		pi.flags |= AUDIO_PCM_PLAYBACK;
	if (p->hw[AUDIO_DIR_CAPTURE].formats)
		pi.flags |= AUDIO_PCM_CAPTURE;
	if (f->pcm == p)
		pi.flags |= AUDIO_PCM_SELECTED;
	for (int d = 0; d < 2; d++) {
		struct snd_pcm_substream *ss = &p->sub[d];
		if (__atomic_load_n(&ss->opened, __ATOMIC_ACQUIRE) &&
		    !(f->pcm == p && (f->dirs & (1u << d))))
			pi.flags |= AUDIO_PCM_BUSY;
	}
	pi.output_id = p->output_id;
	pi.output_type = p->output_type;
	for (size_t i = 0; i + 1 < sizeof(pi.name) && p->name[i]; i++)
		pi.name[i] = p->name[i];
	if (snd_user_out(argp, &pi, sizeof(pi)))
		return -EFAULT;
	return 0;
}

/* Move an open, idle handle to another PCM device of its card, keeping the
 * directions it has.  The caller has entered the handle once. */
static long snd_pcm_select_ioctl(struct snd_file *f, void *argp)
{
	struct snd_card *card = f->card;
	struct snd_pcm *np, *op = f->pcm;
	uint32_t index, dirs = f->dirs, got = 0;
	int n = card->npcm < SND_MAX_PCMS ? card->npcm : SND_MAX_PCMS;
	int expected = 0;
	long rc = 0;

	if (!op)
		return -ENOTTY;
	if (snd_user_in(&index, argp, sizeof(index)))
		return -EFAULT;
	if (index >= (uint32_t)n || !(np = card->pcm[index]))
		return -EINVAL;
	if (np == op)
		return 0;
	if ((snd_pcm_dirs(np) & dirs) != dirs)
		return -ENODEV;

	/* Fence off every other entry into the handle. */
	if (!__atomic_compare_exchange_n(&f->switching, &expected, 1, 0,
					 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
		return -EBUSY;
	if (__atomic_load_n(&f->active, __ATOMIC_SEQ_CST) != 1) {
		rc = -EBUSY;
		goto out;
	}
	/* Only an idle, unmapped stream may move. */
	for (int d = 0; d < 2; d++) {
		struct snd_pcm_substream *ss = &op->sub[d];
		uint32_t st;
		if (!(dirs & (1u << d)))
			continue;
		st = __atomic_load_n(&ss->state, __ATOMIC_ACQUIRE);
		if (st != AUDIO_STATE_OPEN && st != AUDIO_STATE_SETUP &&
		    st != AUDIO_STATE_PREPARED && st != AUDIO_STATE_XRUN) {
			rc = -EBUSY;
			goto out;
		}
		if (ss->mmap_count) {
			rc = -EBUSY;
			goto out;
		}
	}
	/* Claim the new device first so a busy one leaves the handle as it
	 * was. */
	for (int d = 0; d < 2; d++) {
		if (!(dirs & (1u << d)))
			continue;
		rc = snd_pcm_claim(f, &np->sub[d], f->uid);
		if (rc) {
			for (int e = 0; e < d; e++)
				if (got & (1u << e))
					snd_pcm_release_ss(f, &np->sub[e]);
			goto out;
		}
		got |= 1u << d;
	}
	for (int d = 0; d < 2; d++)
		if (dirs & (1u << d))
			snd_pcm_release_ss(f, &op->sub[d]);
	f->pcm = np;
	f->primary = (dirs & (1u << AUDIO_DIR_PLAYBACK)) ? AUDIO_DIR_PLAYBACK :
							    AUDIO_DIR_CAPTURE;
	snd_oss_file_init(f);
	/* Wake pollers sleeping on the old device so they re-poll. */
	for (int d = 0; d < 2; d++)
		if (dirs & (1u << d))
			poll_notify_wq(&op->sub[d].wq);
	kprintf("[snd] card%d: handle moved to pcm%u (%s)\n", card->index,
		index, np->name);
out:
	__atomic_store_n(&f->switching, 0, __ATOMIC_SEQ_CST);
	return rc;
}

static int snd_node_open(struct devfs_node *node, vfs_file_t *file, int flags,
			 struct task *cur)
{
	struct snd_card *card = node ? node->priv : NULL;
	struct snd_card_priv *cp;
	struct snd_file *f;
	int acc = flags & 3;
	uint64_t fl;

	if (!card)
		return -ENODEV;
	cp = snd_card_priv(card);
	if (cp->magic != SND_CARD_MAGIC || card->disconnected || !card->registered)
		return -ENODEV;

	f = kalloc(sizeof(*f));
	if (!f)
		return -ENOMEM;
	mm_memset(f, 0, sizeof(*f));
	f->magic = SND_FILE_MAGIC;
	f->card = card;
	f->vfs = file;
	f->uid = cur ? cur->cred.euid : 0;
	f->can_write = (acc == O_WRONLY || acc == O_RDWR);
	if (node == &card->node_mixer)
		f->kind = SND_NODE_MIXER;
	else if (node == &card->node_dsp)
		f->kind = SND_NODE_DSP;
	else
		f->kind = SND_NODE_AUDIO;

	spin_lock_irqsave(&card->ev_lock, &fl);
	f->ev_seq = card->ev_head;
	spin_unlock_irqrestore(&card->ev_lock, fl);

	if (f->kind != SND_NODE_MIXER) {
		uint32_t want;
		struct snd_pcm *pcm;

		if (acc == O_WRONLY)
			want = 1u << AUDIO_DIR_PLAYBACK;
		else if (acc == O_RDONLY)
			want = 1u << AUDIO_DIR_CAPTURE;
		else
			want = (1u << AUDIO_DIR_PLAYBACK) | (1u << AUDIO_DIR_CAPTURE);

		pcm = snd_pick_pcm(card, want);
		if (!pcm && acc == O_RDWR) {
			/* Read-write on a one-way device: take what exists,
			 * playback preferred. */
			pcm = snd_pick_pcm(card, 1u << AUDIO_DIR_PLAYBACK);
			want = 1u << AUDIO_DIR_PLAYBACK;
			if (!pcm) {
				pcm = snd_pick_pcm(card, 1u << AUDIO_DIR_CAPTURE);
				want = 1u << AUDIO_DIR_CAPTURE;
			}
		}
		if (!pcm) {
			kfree(f);
			return -ENODEV;
		}
		f->pcm = pcm;
		for (int d = 0; d < 2; d++) {
			if (!(want & (1u << d)))
				continue;
			int rc = snd_pcm_claim(f, &pcm->sub[d], f->uid);
			if (rc) {
				for (int e = 0; e < d; e++)
					if (f->dirs & (1u << e))
						snd_pcm_release_ss(f, &pcm->sub[e]);
				f->magic = 0;
				kfree(f);
				return rc;
			}
			f->dirs |= 1u << d;
		}
		f->primary = (f->dirs & (1u << AUDIO_DIR_PLAYBACK)) ?
				     AUDIO_DIR_PLAYBACK :
				     AUDIO_DIR_CAPTURE;
		snd_oss_file_init(f);
	}

	snd_card_ref(card);
	__atomic_fetch_add(&cp->open_files, 1, __ATOMIC_ACQ_REL);
	device_file_set_priv(file, f);
	return 0;
}

static void snd_node_release(vfs_file_t *file)
{
	struct snd_file *f = snd_file_of(file);
	struct snd_card *card;

	if (!f)
		return;
	card = f->card;
	if (f->pcm) {
		for (int d = 0; d < 2; d++)
			if (f->dirs & (1u << d))
				snd_pcm_release_ss(f, &f->pcm->sub[d]);
	}
	f->dirs = 0;
	f->magic = 0;
	device_file_set_priv(file, NULL);
	kfree(f);
	__atomic_fetch_sub(&snd_card_priv(card)->open_files, 1, __ATOMIC_ACQ_REL);
	snd_card_unref(card);
}

static long snd_node_read_in(vfs_file_t *file, void *buf, long bytes,
			     int nonblock)
{
	struct snd_file *f = snd_file_of(file);
	struct snd_pcm_substream *ss;

	if (!f)
		return -EBADF;
	if (f->card->disconnected)
		return -ENODEV;
	if (f->kind == SND_NODE_MIXER)
		return -EINVAL;
	if (bytes < 0)
		return -EINVAL;
	if (bytes == 0)
		return 0;
	if (f->kind == SND_NODE_DSP || f->oss_used)
		return snd_oss_read(f, buf, bytes, nonblock);
	ss = snd_file_ss(f, AUDIO_DIR_CAPTURE);
	if (!ss)
		return -EINVAL;
	return snd_pcm_transfer(ss, buf, bytes, nonblock, 1, 1);
}

static long snd_node_write_in(vfs_file_t *file, const void *buf, long bytes,
			      int nonblock)
{
	struct snd_file *f = snd_file_of(file);
	struct snd_pcm_substream *ss;

	if (!f)
		return -EBADF;
	if (f->card->disconnected)
		return -ENODEV;
	if (f->kind == SND_NODE_MIXER)
		return -EINVAL;
	if (bytes < 0)
		return -EINVAL;
	if (bytes == 0)
		return 0;
	if (f->kind == SND_NODE_DSP || f->oss_used)
		return snd_oss_write(f, buf, bytes, nonblock);
	ss = snd_file_ss(f, AUDIO_DIR_PLAYBACK);
	if (!ss)
		return -EINVAL;
	return snd_pcm_transfer(ss, (void *)buf, bytes, nonblock, 1, 1);
}

static long snd_node_ioctl_in(struct snd_file *f, unsigned long req,
			      void *argp)
{
	unsigned type = _IOC_TYPE(req);
	unsigned nr = _IOC_NR(req);

	switch (type) {
	case AUDIO_IOC_TYPE:
		if (req == AUDIO_GETPCMINFO || req == AUDIO_SELECTPCM) {
			if (!f->pcm)
				return -ENOTTY;
			if (f->card->disconnected)
				return -ENODEV;
			return req == AUDIO_GETPCMINFO ?
				       snd_pcm_info_ioctl(f, argp) :
				       snd_pcm_select_ioctl(f, argp);
		}
		if (nr >= 0x82 && nr <= 0x8f) {
			if (!f->pcm)
				return -ENOTTY;
			if (f->card->disconnected)
				return -ENODEV;
			return snd_pcm_ioctl(f, req, argp);
		}
		return snd_ctl_ioctl(f, req, argp);
	case 'P':
		if (!f->pcm)
			return -ENOTTY;
		if (f->card->disconnected)
			return -ENODEV;
		return snd_oss_dsp_ioctl(f, req, argp);
	case 'M':
		return snd_oss_mixer_ioctl(f, req, argp);
	case 'X':
		return snd_oss_sysinfo_ioctl(f, req, argp);
	default:
		return -ENOTTY;
	}
}

/* Entry points: each call counts itself in the handle (see
 * snd_file_enter) so AUDIO_SELECTPCM never swaps the PCM under it. */
static long snd_node_read(vfs_file_t *file, void *buf, long bytes, int nonblock)
{
	struct snd_file *f = snd_file_of(file);
	long rc;

	if (!f)
		return -EBADF;
	if (!snd_file_enter(f))
		return -EBUSY;
	rc = snd_node_read_in(file, buf, bytes, nonblock);
	snd_file_leave(f);
	return rc;
}

static long snd_node_write(vfs_file_t *file, const void *buf, long bytes,
			   int nonblock)
{
	struct snd_file *f = snd_file_of(file);
	long rc;

	if (!f)
		return -EBADF;
	if (!snd_file_enter(f))
		return -EBUSY;
	rc = snd_node_write_in(file, buf, bytes, nonblock);
	snd_file_leave(f);
	return rc;
}

static long snd_node_ioctl(vfs_file_t *file, unsigned long req, void *argp,
			   struct task *cur)
{
	struct snd_file *f = snd_file_of(file);
	long rc;
	(void)cur;

	if (!f)
		return -EBADF;
	if (!snd_file_enter(f))
		return -EBUSY;
	rc = snd_node_ioctl_in(f, req, argp);
	snd_file_leave(f);
	return rc;
}

static int snd_node_mmap_in(vfs_file_t *file, struct device_mmap *m)
{
	struct snd_file *f = snd_file_of(file);

	if (!f)
		return -EBADF;
	if (!f->pcm)
		return -ENODEV;
	if (f->card->disconnected)
		return -ENODEV;
	/* OSS programs map right after setting the format: apply it. */
	if (f->kind == SND_NODE_DSP || f->oss_used) {
		int rc = snd_oss_configure(f);
		if (rc)
			return rc;
	}
	return snd_pcm_mmap(f, m);
}

static int snd_node_mmap(vfs_file_t *file, struct device_mmap *m)
{
	struct snd_file *f = snd_file_of(file);
	int rc;

	if (!f)
		return -EBADF;
	if (!snd_file_enter(f))
		return -EBUSY;
	rc = snd_node_mmap_in(file, m);
	snd_file_leave(f);
	return rc;
}

static short snd_node_poll_in(vfs_file_t *file, short events,
			      struct poll_table *pt)
{
	struct snd_file *f = snd_file_of(file);
	short mask = 0;

	if (!f)
		return POLLERR;
	poll_wait(pt, file, &f->card->ev_wq);
	if (f->pcm) {
		for (int d = 0; d < 2; d++) {
			struct snd_pcm_substream *ss = snd_file_ss(f, d);
			if (!ss)
				continue;
			poll_wait(pt, file, &ss->wq);
			/* OSS: a select() for input starts recording when the
			 * input trigger is enabled. */
			if (d == AUDIO_DIR_CAPTURE && f->kind == SND_NODE_DSP &&
			    (events & (POLLIN | POLLRDNORM)) &&
			    (f->oss_trigger & PCM_ENABLE_INPUT)) {
				if (snd_oss_configure(f) == 0 &&
				    __atomic_load_n(&ss->state, __ATOMIC_ACQUIRE) ==
					    AUDIO_STATE_PREPARED)
					(void)snd_pcm_start(ss);
			}
			mask |= snd_pcm_poll_ss(ss, events, f->kind == SND_NODE_DSP);
		}
	}
	if (snd_ctl_event_pending(f))
		mask |= POLLPRI;
	if (f->card->disconnected)
		mask |= POLLHUP | POLLERR;
	return mask & (events | POLLERR | POLLHUP);
}

static short snd_node_poll(vfs_file_t *file, short events,
			   struct poll_table *pt)
{
	struct snd_file *f = snd_file_of(file);
	short mask;

	if (!f)
		return POLLERR;
	if (!snd_file_enter(f)) {
		/* The handle is being moved to another PCM: report it ready
		 * so the caller comes straight back. */
		return events & (POLLOUT | POLLWRNORM | POLLIN | POLLRDNORM);
	}
	mask = snd_node_poll_in(file, events, pt);
	snd_file_leave(f);
	return mask;
}

static const struct device_ops snd_node_ops = {
	.open = snd_node_open,
	.release = snd_node_release,
	.read = snd_node_read,
	.write = snd_node_write,
	.ioctl = snd_node_ioctl,
	.mmap = snd_node_mmap,
	.poll = snd_node_poll,
};

/* ======================================================================
 * Registration
 * ====================================================================== */

static int snd_register_node(struct snd_card *card, int which,
			     struct devfs_node *n, char *path_out,
			     size_t path_cap, const char *base, uint32_t fn)
{
	struct snd_card_priv *cp = snd_card_priv(card);
	char *sysname = cp->sysname[which];
	int idx = card->index;

	/* Card 0 keeps the conventional unnumbered names for the OSS nodes
	 * (/dev/dsp, /dev/mixer); its native node is /dev/audio0 so every
	 * card's native node has the same form.  The devfs table is small,
	 * so no further aliases are made. */
	if (idx == 0 && fn != AUDIO_MINOR_AUDIO)
		ksnprintf(sysname, sizeof(cp->sysname[0]), "%s", base);
	else
		ksnprintf(sysname, sizeof(cp->sysname[0]), "%s%d", base, idx);

	mm_memset(n, 0, sizeof(*n));
	ksnprintf(n->path, sizeof(n->path), "/dev/%s", sysname);
	n->mode = 0660;
	n->uid = 0;
	n->gid = DEVFS_GID_AUDIO;
	n->major = AUDIO_MAJOR;
	n->minor = ((uint32_t)idx << 4) | fn;
	n->ops = &snd_node_ops;
	n->priv = card;
	snd_strlcpy(path_out, n->path, path_cap);

	int rc = device_register(n);
	if (rc) {
		kprintf("[snd] card%d: cannot create %s (%d)%s\n", idx, n->path, rc,
			rc == -ENOSPC ? ": the device node table is full" : "");
		path_out[0] = 0;
		return rc;
	}
	cp->node_ok[which] = 1;
	sysfs_add_char_device(sysname, AUDIO_MAJOR, n->minor, "sound", card->pci);
	return 0;
}

int snd_card_register(struct snd_card *card)
{
	struct snd_card_priv *cp;
	uint64_t fl;
	int slot = -1;
	int has_pcm = 0;

	if (!card)
		return -EINVAL;
	cp = snd_card_priv(card);
	if (cp->magic != SND_CARD_MAGIC)
		return -EINVAL;
	if (card->registered)
		return -EBUSY;
	if (card->npcm < 0 || card->npcm > SND_MAX_PCMS)
		return -EINVAL;
	for (int i = 0; i < card->npcm; i++)
		if (card->pcm[i])
			has_pcm = 1;
	if (!has_pcm && !card->ctl) {
		kprintf("[snd] %s: refusing to register a card with neither PCM devices nor controls\n",
			card->driver);
		return -EINVAL;
	}

	spin_lock_irqsave(&g_cards_lock, &fl);
	for (int i = 0; i < SND_MAX_CARDS; i++) {
		if (!g_cards[i]) {
			slot = i;
			g_cards[i] = card;
			break;
		}
	}
	spin_unlock_irqrestore(&g_cards_lock, fl);
	if (slot < 0) {
		kprintf("[snd] %s: no free card slot (%d cards registered)\n",
			card->driver, SND_MAX_CARDS);
		return -ENOSPC;
	}
	cp->slot = slot;
	card->index = slot;
	card->name[sizeof(card->name) - 1] = 0;
	card->codec[sizeof(card->codec) - 1] = 0;
	card->driver[sizeof(card->driver) - 1] = 0;
	card->bus[sizeof(card->bus) - 1] = 0;
	card->registered = 1;

	/* The capability flags the core knows of itself. */
	for (int i = 0; i < card->npcm; i++) {
		struct snd_pcm *p = card->pcm[i];
		if (!p)
			continue;
		if (p->hw[AUDIO_DIR_PLAYBACK].formats)
			card->info_flags |= AUDIO_INFO_PLAYBACK;
		if (p->hw[AUDIO_DIR_CAPTURE].formats)
			card->info_flags |= AUDIO_INFO_CAPTURE;
	}
	if (has_pcm)
		card->info_flags |= AUDIO_INFO_MMAP;

	if (has_pcm) {
		snd_register_node(card, 2, &card->node_audio, card->path_audio,
				  sizeof(card->path_audio), "audio",
				  AUDIO_MINOR_AUDIO);
		snd_register_node(card, 1, &card->node_dsp, card->path_dsp,
				  sizeof(card->path_dsp), "dsp", AUDIO_MINOR_DSP);
	}
	snd_register_node(card, 0, &card->node_mixer, card->path_mixer,
			  sizeof(card->path_mixer), "mixer", AUDIO_MINOR_MIXER);

	if (card->codec[0])
		kprintf("[snd] card%d: %s (%s) at %s -> %s\n", slot, card->name,
			card->codec, card->bus,
			card->path_audio[0] ? card->path_audio : card->path_mixer);
	else
		kprintf("[snd] card%d: %s at %s -> %s\n", slot, card->name,
			card->bus,
			card->path_audio[0] ? card->path_audio : card->path_mixer);
	for (int i = 0; i < card->npcm; i++) {
		struct snd_pcm *p = card->pcm[i];
		if (!p)
			continue;
		const struct snd_pcm_hw *ph = &p->hw[AUDIO_DIR_PLAYBACK];
		const struct snd_pcm_hw *ch = &p->hw[AUDIO_DIR_CAPTURE];
		kprintf("[snd] card%d: pcm%d \"%s\":%s%s formats 0x%x/0x%x rates 0x%x/0x%x channels %u-%u\n",
			slot, i, p->name, ph->formats ? " playback" : "",
			ch->formats ? " capture" : "", ph->formats, ch->formats,
			ph->rates, ch->rates,
			ph->formats ? ph->channels_min : ch->channels_min,
			ph->formats ? ph->channels_max : ch->channels_max);
	}
	return 0;
}

void snd_card_disconnect(struct snd_card *card)
{
	struct snd_card_priv *cp;

	if (!card)
		return;
	cp = snd_card_priv(card);
	if (cp->magic != SND_CARD_MAGIC)
		return;
	if (__atomic_exchange_n(&card->disconnected, 1, __ATOMIC_ACQ_REL))
		return;
	snd_pcm_disconnect_all(card);
	snd_card_event(card, AUDIO_EV_DISCONNECT, (uint32_t)card->index, 0);
	/* Nobody opens the nodes from here on; handles already open see
	 * POLLHUP and -ENODEV and keep the card's memory until they close. */
	snd_card_unregister_nodes(card);
	kprintf("[snd] card%d: %s disconnected\n", card->index, card->name);
}

/* ======================================================================
 * Display audio (display driver <-> HDMI/DP codec)
 * ====================================================================== */

static const struct snd_display_audio_ops *g_disp_ops;
static void *g_disp_obj;
static spinlock_t g_disp_lock = SPINLOCK_INIT("snd_disp");

void snd_display_audio_register(const struct snd_display_audio_ops *ops,
				void *display)
{
	uint64_t fl;

	spin_lock_irqsave(&g_disp_lock, &fl);
	g_disp_ops = ops;
	g_disp_obj = ops ? display : NULL;
	spin_unlock_irqrestore(&g_disp_lock, fl);
	kprintf("[snd] core: display audio %s\n",
		ops ? "hooks registered" : "hooks removed");
}

const struct snd_display_audio_ops *snd_display_audio_ops(void **display)
{
	const struct snd_display_audio_ops *ops;
	uint64_t fl;

	spin_lock_irqsave(&g_disp_lock, &fl);
	ops = g_disp_ops;
	if (display)
		*display = g_disp_obj;
	spin_unlock_irqrestore(&g_disp_lock, fl);
	return ops;
}

/* The notification is carried to ksnd: the display driver may call from
 * its interrupt or hotplug path, and the codec side sends verbs, which
 * waits on the link. */
static void snd_display_notify_work(void *arg)
{
	uint64_t v = (uint64_t)arg;
	int port = (int)(v >> 1);
	int present = (int)(v & 1);

	hda_hdmi_notify(port, present);
}

void snd_display_audio_notify(int port, int present)
{
	if (port < 0 || port > 0xffff)
		return;
	uint64_t v = ((uint64_t)(uint32_t)port << 1) | (present ? 1u : 0u);
	if (snd_schedule_work(snd_display_notify_work, (void *)v, 0) != 0)
		kprintf("[snd] core: display audio notification for port %d lost\n",
			port);
}

/* ======================================================================
 * Entry points
 * ====================================================================== */

static int g_snd_inited;
static int g_snd_late_done;

void snd_init(void)
{
	int hda, ac97, es1371, sof;

	if (g_snd_inited)
		return;
	g_snd_inited = 1;
	snd_work_init_once();
	kprintf("[snd] core: probing sound hardware\n");

	hda = hda_probe_all();
	ac97 = ac97_probe_all();
	es1371 = es1371_probe_all();
	sof = sof_probe_all();
	if (hda < 0)
		hda = 0;
	if (ac97 < 0)
		ac97 = 0;
	if (es1371 < 0)
		es1371 = 0;
	if (sof < 0)
		sof = 0;

	int total = 0;
	for (int i = 0; i < SND_MAX_CARDS; i++)
		if (snd_card_get(i))
			total++;
	if (total == 0)
		kprintf("[snd] core: no sound hardware found\n");
	else
		kprintf("[snd] core: %d card%s registered (hda %d, ac97 %d, es1371 %d, sof %d)\n",
			total, total == 1 ? "" : "s", hda, ac97, es1371, sof);
}

void snd_late_init(void)
{
	if (g_snd_late_done)
		return;
	g_snd_late_done = 1;
	snd_work_init_once();
	if (!g_ksnd) {
		g_ksnd = sched_add_task(ksnd_main, NULL, g_ksnd_stack,
					sizeof(g_ksnd_stack));
		if (!g_ksnd)
			kprintf("[snd] core: cannot start the ksnd thread; deferred work will not run\n");
		else
			wq_wake_all(&g_work_wq); /* anything queued during probe */
	}
	hda_late_init();
	sof_late_init();
}
