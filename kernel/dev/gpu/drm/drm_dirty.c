// LikeOS-64 -- GEM object dirty tracking for coherent mappings.
//
// A client that maps a buffer coherently writes through the mapping and
// announces nothing: no ioctl, no command.  The device keeps a second copy
// of the pages and has to be told which of them changed, and the processor
// already knows -- every write leaves the dirty bit in the entry that
// mapped it, and a write against an entry whose write bit was taken away
// raises a fault that names the page.  This file turns those two records
// into one the driver can consume, per object, as a bitmap of dirty pages.
//
// The shape is the reference implementation's, ported to this kernel's
// address spaces, and it tracks by one of two methods per object:
//
//   PAGETABLE -- leave the mapping writable and, once per submission,
//   sweep the entries for hardware dirty bits, clearing as it goes.  Cheap
//   for an object rewritten wholesale every frame; the sweep costs one
//   pass either way and no write ever faults.
//
//   MKWRITE -- take the write bit away and let the first write to each
//   page fault; the fault records the page and hands the bit back.  Cheap
//   for a large object touched sparsely; pages nobody writes cost nothing.
//
// An object starts in the method its size suggests and switches when the
// evidence says the other would be cheaper: repeated sweeps that find
// nothing argue for faulting, repeated frames that fault in more than a
// tenth of the object argue for sweeping.  The counters and thresholds
// match the reference implementation.
//
// What is deliberately different, and why:
//
//   - The reference walks every mapping of the object through the shared
//     file's reverse map.  This kernel has no such map, so the sweeps walk
//     the SUBMITTING address space's region records instead -- which is
//     every mapping there is, in the only case that occurs: the process
//     that mapped the buffer is the process that submits.  Mappings the
//     sweep cannot reach (another process's, after a fork; an exported
//     buffer mapped through its descriptor) are detected by comparing the
//     records walked against the records that exist, and answered by
//     reporting the WHOLE object dirty until they are gone.  Coarse, and
//     never wrong: the cost is bandwidth, exactly what every submission
//     paid unconditionally before any of this existed.
//
//   - A mapping that disappears (unmap, exit of a forked child) may take
//     unswept dirty bits with it.  The unmap path hands written pages to
//     the tracker first (mm_dirty_ops.page_dirty), and anything that path
//     cannot see is covered by the same full-object answer, latched when a
//     mapping record is dropped while tracking is live.
//
// Two rules carried over unchanged, because each was once a corruption:
// entries are only ever changed with atomic exchanges (the processor sets
// dirty bits with locked cycles of its own, and a plain read-modify-write
// races them and loses writes), and no sweep's result is trusted until the
// translations it changed are gone from every processor (a stale one lets
// writes through unrecorded).  Both live in mm_dirty_*_mappings(); see the
// comment there.

#include <kernel/dev/gpu/drm.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/sched.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

enum drm_gem_dirty_method {
	DRM_GEM_DIRTY_PAGETABLE,
	DRM_GEM_DIRTY_MKWRITE,
};

/* Consecutive triggers before the method changes: sweeps that found
 * nothing (toward MKWRITE), or frames that faulted in more than
 * DRM_GEM_DIRTY_PERCENTAGE of the object (toward PAGETABLE). */
#define DRM_GEM_DIRTY_CHANGE_TRIGGERS 2
#define DRM_GEM_DIRTY_PERCENTAGE 10

/* Below this many pages the sweep is cheaper than any amount of faulting:
 * one page table's worth of entries, read linearly. */
#define DRM_GEM_DIRTY_PAGETABLE_LIMIT 512

struct drm_gem_dirty {
	enum drm_gem_dirty_method method;
	unsigned int change_count;
	int ref_count;
	/* Report the whole object dirty at the next scan: a mapping went
	 * where the sweep cannot follow, or vanished with its record. */
	int full;
	uint64_t start, end; /* [start, end) brackets every set bit */
	uint64_t bitmap_size; /* pages */
	uint64_t bitmap[];
};

/* One lock for every tracker's state and every object's ->dirty pointer.
 *
 * Global rather than per-object because the fault path must be able to
 * find out whether an object still HAS a tracker, and a lock inside the
 * tracker cannot answer that -- it dies with it.  The critical sections
 * are a few loads and stores; the traffic is one fault per page per frame
 * at worst and a handful of scans per submission.
 *
 * Never held across the mm walks (they sleep); the walks report back
 * through callbacks that take it per page.  A scan may therefore hold a
 * tracker pointer while not holding the lock -- that is safe only because
 * every scan runs on behalf of a submission that holds a reference on the
 * resource holding the tracking reference, so release cannot get to zero
 * underneath it. */
static spinlock_t g_dirty_lock = SPINLOCK_INIT("drm_dirty");

/* ---- small bitmap helpers ---------------------------------------------- */

#define BITS_PER_WORD 64

static inline void bit_set(uint64_t *map, uint64_t n)
{
	map[n / BITS_PER_WORD] |= 1ULL << (n % BITS_PER_WORD);
}

static inline int bit_test(const uint64_t *map, uint64_t n)
{
	return (map[n / BITS_PER_WORD] >> (n % BITS_PER_WORD)) & 1;
}

static void bits_clear_range(uint64_t *map, uint64_t start, uint64_t n)
{
	for (uint64_t i = start; i < start + n; i++)
		map[i / BITS_PER_WORD] &= ~(1ULL << (i % BITS_PER_WORD));
}

static void bits_set_range(uint64_t *map, uint64_t start, uint64_t n)
{
	for (uint64_t i = start; i < start + n; i++)
		bit_set(map, i);
}

static uint64_t bits_next_set(const uint64_t *map, uint64_t size,
			      uint64_t from)
{
	for (uint64_t i = from; i < size; i++) {
		/* Skip empty words wholesale; the tail is walked bit-wise. */
		if ((i % BITS_PER_WORD) == 0 && map[i / BITS_PER_WORD] == 0) {
			i += BITS_PER_WORD - 1;
			continue;
		}
		if (bit_test(map, i))
			return i;
	}
	return size;
}

static uint64_t bits_next_clear(const uint64_t *map, uint64_t size,
				uint64_t from)
{
	for (uint64_t i = from; i < size; i++) {
		if ((i % BITS_PER_WORD) == 0 &&
		    map[i / BITS_PER_WORD] == ~0ULL) {
			i += BITS_PER_WORD - 1;
			continue;
		}
		if (!bit_test(map, i))
			return i;
	}
	return size;
}

/* ---- the walk plumbing -------------------------------------------------- */

/* Record one dirty page, from the clean sweep's callback or the fault. */
static void dirty_record_page(struct drm_gem_dirty *d, uint64_t page)
{
	if (page >= d->bitmap_size)
		return;
	bit_set(d->bitmap, page);
	if (page < d->start)
		d->start = page;
	if (page + 1 > d->end)
		d->end = page + 1;
}

static void dirty_record_cb(void *arg, uint64_t page)
{
	struct drm_gem_dirty *d = arg;
	uint64_t fl;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	dirty_record_page(d, page);
	spin_unlock_irqrestore(&g_dirty_lock, fl);
}

/* A sweep over the submitting address space's mappings of `o'.  Returns
 * the entries changed; *foreign is set when records exist that the sweep
 * could not reach, which the caller answers with a full-object report. */
static uint64_t dirty_walk(struct drm_gem_object *o, struct drm_gem_dirty *d,
			   int clean, uint64_t first, uint64_t last,
			   int *foreign)
{
	struct mm_dirty_walk w;

	mm_memset(&w, 0, sizeof(w));
	w.obj = o;
	w.ops = &drm_gem_dirty_mmap_ops;
	w.file_base = drm_gem_mmap_offset(o);
	w.npages = d->bitmap_size;
	w.first = first;
	w.last = last;
	if (clean) {
		w.record = dirty_record_cb;
		w.arg = d;
	}
	if (clean)
		mm_dirty_clean_mappings(&w);
	else
		mm_dirty_wp_mappings(&w);
	if ((int)w.matched != __atomic_load_n(&o->map_records,
					      __ATOMIC_RELAXED))
		*foreign = 1;
	return w.marked;
}

/* ---- the two methods ---------------------------------------------------- */

static void dirty_scan_pagetable(struct drm_gem_object *o,
				 struct drm_gem_dirty *d, int *foreign)
{
	uint64_t marked;
	uint64_t fl;

	marked = dirty_walk(o, d, 1, 0, d->bitmap_size, foreign);

	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (marked == 0)
		d->change_count++;
	else
		d->change_count = 0;
	if (d->change_count <= DRM_GEM_DIRTY_CHANGE_TRIGGERS) {
		spin_unlock_irqrestore(&g_dirty_lock, fl);
		return;
	}
	/* Sweeps keep finding nothing: stop paying for them and let the
	 * next write to each page announce itself instead. */
	d->change_count = 0;
	d->method = DRM_GEM_DIRTY_MKWRITE;
	spin_unlock_irqrestore(&g_dirty_lock, fl);

	dirty_walk(o, d, 0, 0, d->bitmap_size, foreign);
	dirty_walk(o, d, 1, 0, d->bitmap_size, foreign);
}

static void dirty_scan_mkwrite(struct drm_gem_object *o,
			       struct drm_gem_dirty *d, int *foreign)
{
	uint64_t start, end, marked;
	uint64_t fl;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	start = d->start;
	end = d->end;
	spin_unlock_irqrestore(&g_dirty_lock, fl);

	if (end <= start) {
		/* Nothing faulted in since the last consumption -- but the
		 * record census still has to run, or a mapping in another
		 * address space (whose writes fault into the bitmap of a
		 * tracker whose pages are writable THERE) would go
		 * unnoticed for as long as nothing faulted here. */
		dirty_walk(o, d, 0, 0, 0, foreign);
		return;
	}

	/* Take the write bit back from the pages handed out since the last
	 * scan, so the next write to each announces itself again.  Only
	 * the bracketed range: nothing outside it was handed out. */
	marked = dirty_walk(o, d, 0, start, end, foreign);

	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (100ULL * marked / d->bitmap_size > DRM_GEM_DIRTY_PERCENTAGE)
		d->change_count++;
	else
		d->change_count = 0;
	if (d->change_count <= DRM_GEM_DIRTY_CHANGE_TRIGGERS) {
		spin_unlock_irqrestore(&g_dirty_lock, fl);
		return;
	}
	/* Most of the object faults in every frame: the sweep is cheaper
	 * than the faults.  Clean the whole range so the sweep starts from
	 * nothing, then mark everything the faults had bracketed -- pages
	 * in that range held the write bit for a while, and what they took
	 * through it has to be assumed, not proven. */
	d->method = DRM_GEM_DIRTY_PAGETABLE;
	d->change_count = 0;
	spin_unlock_irqrestore(&g_dirty_lock, fl);

	dirty_walk(o, d, 1, 0, d->bitmap_size, foreign);

	spin_lock_irqsave(&g_dirty_lock, &fl);
	bits_clear_range(d->bitmap, 0, d->bitmap_size);
	if (d->start < d->end)
		bits_set_range(d->bitmap, d->start, d->end - d->start);
	spin_unlock_irqrestore(&g_dirty_lock, fl);
}

/* ---- the interface the driver uses -------------------------------------- */

void drm_gem_dirty_scan(struct drm_gem_object *o)
{
	struct drm_gem_dirty *d;
	int foreign = 0;
	uint64_t fl;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	d = o->dirty;
	if (d && d->full) {
		d->full = 0;
		foreign = 1; /* answered the same way: everything */
	}
	spin_unlock_irqrestore(&g_dirty_lock, fl);
	if (!d)
		return;

	if (d->method == DRM_GEM_DIRTY_PAGETABLE)
		dirty_scan_pagetable(o, d, &foreign);
	else
		dirty_scan_mkwrite(o, d, &foreign);

	if (foreign) {
		/* Mappings exist that the sweep could not reach, or one
		 * vanished with its record: every page might have been
		 * written, so every page is reported.  Costly and correct;
		 * see the header comment. */
		spin_lock_irqsave(&g_dirty_lock, &fl);
		bits_set_range(d->bitmap, 0, d->bitmap_size);
		d->start = 0;
		d->end = d->bitmap_size;
		spin_unlock_irqrestore(&g_dirty_lock, fl);
	}
}

void drm_gem_dirty_transfer(struct drm_gem_object *o,
			    void (*cb)(void *arg, uint64_t first,
				       uint64_t last),
			    void *arg)
{
	struct drm_gem_dirty *d;
	uint64_t fl;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	d = o->dirty;
	if (!d || d->end <= d->start) {
		spin_unlock_irqrestore(&g_dirty_lock, fl);
		return;
	}
	/* Runs of set bits become calls; the callback must not sleep (it
	 * runs under the tracker lock and only shapes driver structures). */
	uint64_t cur = d->start;
	uint64_t limit = d->end;

	while (cur < limit) {
		uint64_t first = bits_next_set(d->bitmap, limit, cur);

		if (first >= limit)
			break;
		uint64_t last = bits_next_clear(d->bitmap, limit, first + 1);

		bits_clear_range(d->bitmap, first, last - first);
		cb(arg, first, last);
		cur = last + 1;
	}
	d->start = d->bitmap_size;
	d->end = 0;
	spin_unlock_irqrestore(&g_dirty_lock, fl);
}

int drm_gem_dirty_add(struct drm_gem_object *o)
{
	struct drm_gem_dirty *d;
	uint64_t pages = o->npages;
	uint64_t fl;
	int foreign = 0;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (o->dirty) {
		o->dirty->ref_count++;
		spin_unlock_irqrestore(&g_dirty_lock, fl);
		return 0;
	}
	spin_unlock_irqrestore(&g_dirty_lock, fl);

	if (!pages)
		return -EINVAL;
	d = kalloc(sizeof(*d) +
		   ((pages + BITS_PER_WORD - 1) / BITS_PER_WORD) *
			   sizeof(uint64_t));
	if (!d)
		return -ENOMEM;
	mm_memset(d, 0,
		  sizeof(*d) + ((pages + BITS_PER_WORD - 1) / BITS_PER_WORD) *
				       sizeof(uint64_t));
	d->bitmap_size = pages;
	d->start = pages;
	d->end = 0;
	d->ref_count = 1;
	d->method = (pages < DRM_GEM_DIRTY_PAGETABLE_LIMIT) ?
			    DRM_GEM_DIRTY_PAGETABLE :
			    DRM_GEM_DIRTY_MKWRITE;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (o->dirty) {
		/* Two creators raced; the first one's tracker stands. */
		o->dirty->ref_count++;
		spin_unlock_irqrestore(&g_dirty_lock, fl);
		kfree(d);
		return 0;
	}
	/* Published BEFORE the initial protection pass, so a write fault
	 * arriving mid-pass finds the tracker and is recorded.  One that
	 * slips through before its page is protected leaves the hardware
	 * dirty bit instead, which the pass right after picks up. */
	o->dirty = d;
	spin_unlock_irqrestore(&g_dirty_lock, fl);

	if (d->method == DRM_GEM_DIRTY_MKWRITE) {
		/* Protect everything, then collect what was already dirty:
		 * writes older than the tracker still have to reach the
		 * device once. */
		dirty_walk(o, d, 0, 0, pages, &foreign);
		dirty_walk(o, d, 1, 0, pages, &foreign);
		if (foreign) {
			spin_lock_irqsave(&g_dirty_lock, &fl);
			d->full = 1;
			spin_unlock_irqrestore(&g_dirty_lock, fl);
		}
	}
	return 0;
}

void drm_gem_dirty_release(struct drm_gem_object *o)
{
	struct drm_gem_dirty *d = NULL;
	uint64_t fl;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (o->dirty && --o->dirty->ref_count == 0) {
		d = o->dirty;
		/* Detached under the lock: nothing can reach it again --
		 * the pointer is only ever followed with this lock held.
		 * Entries still write-protected stay so; the next write
		 * faults, finds no tracker, and gets the bit back. */
		o->dirty = NULL;
	}
	spin_unlock_irqrestore(&g_dirty_lock, fl);
	if (d)
		kfree(d);
}

/* ---- the mapping callbacks (mm_dirty_ops) ------------------------------- */

/* A write faulted against a protected page.  Record it -- BEFORE the
 * fault handler hands the write bit back, so a protection pass reading
 * the record afterwards cannot miss the write (see the fault handler).
 * Recorded only in the faulting method; a leftover protected entry from
 * before a method switch just gets its bit back, and the write it lets
 * through leaves the hardware dirty bit for the sweep. */
void drm_gem_dirty_fault_page(struct drm_gem_object *o, uint64_t page)
{
	uint64_t fl;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (o->dirty && o->dirty->method == DRM_GEM_DIRTY_MKWRITE)
		dirty_record_page(o->dirty, page);
	spin_unlock_irqrestore(&g_dirty_lock, fl);
}

static void drm_gem_dirty_mkwrite(void *obj, uint64_t offset)
{
	struct drm_gem_object *o = obj;
	uint64_t base = drm_gem_mmap_offset(o);

	if (offset < base)
		return;
	drm_gem_dirty_fault_page(o, (offset - base) / PAGE_SIZE);
}

/* A written page's entry is about to be discarded by unmap: keep the
 * write.  Any method -- a set bit never hurts, a lost one is a stale
 * device copy. */
void drm_gem_dirty_mark_page(struct drm_gem_object *o, uint64_t page)
{
	uint64_t fl;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (o->dirty)
		dirty_record_page(o->dirty, page);
	spin_unlock_irqrestore(&g_dirty_lock, fl);
}

static void drm_gem_dirty_page_dirty(void *obj, uint64_t offset)
{
	struct drm_gem_object *o = obj;
	uint64_t base = drm_gem_mmap_offset(o);

	if (offset < base)
		return;
	drm_gem_dirty_mark_page(o, (offset - base) / PAGE_SIZE);
}

/* Whether entries of a new mapping must be born write-protected: yes
 * exactly while the object is tracked by faults.  In the sweep method a
 * writable entry is the point -- the write leaves the dirty bit and the
 * sweep collects it.  Exported for the descriptor-mapping flavour too. */
int drm_gem_dirty_wp_new_mapping(struct drm_gem_object *o)
{
	uint64_t fl;
	int wp;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	wp = o->dirty && o->dirty->method == DRM_GEM_DIRTY_MKWRITE;
	spin_unlock_irqrestore(&g_dirty_lock, fl);
	return wp;
}

static int drm_gem_dirty_wp_new(void *obj)
{
	return drm_gem_dirty_wp_new_mapping(obj);
}

const struct mm_dirty_ops drm_gem_dirty_mmap_ops = {
	.mkwrite = drm_gem_dirty_mkwrite,
	.page_dirty = drm_gem_dirty_page_dirty,
	.wp_new_mapping = drm_gem_dirty_wp_new,
};

/* ---- mapping-record census --------------------------------------------- */

/* Called by the mmap get/put wrappers as region records referencing the
 * object come and go: the initial mapping, splits, forked copies.  The
 * count is what the sweeps compare their walk against; the drop latch is
 * what covers a record that took unswept writes with it. */
void drm_gem_dirty_map_note(struct drm_gem_object *o)
{
	__atomic_add_fetch(&o->map_records, 1, __ATOMIC_RELAXED);
}

void drm_gem_dirty_map_drop(struct drm_gem_object *o)
{
	uint64_t fl;

	__atomic_sub_fetch(&o->map_records, 1, __ATOMIC_RELAXED);
	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (o->dirty)
		o->dirty->full = 1;
	spin_unlock_irqrestore(&g_dirty_lock, fl);
}
