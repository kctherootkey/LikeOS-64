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
//     that mapped the buffer is the process that submits -- whether it
//     mapped it through the device node or through an exported descriptor
//     (both flavours of record are walked; see mm_dirty_walk.ops_alt).
//     Mappings the sweep cannot reach (another process's, after a fork or
//     an import) are detected by comparing the records walked against the
//     records that exist, and answered by reporting the WHOLE object dirty
//     until they are gone.  Coarse, and never wrong: the cost is
//     bandwidth, exactly what every submission paid unconditionally
//     before any of this existed.
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

/* The two mistakes do not cost the same, so the two directions are not
 * symmetric.
 *
 * Faulting costs one page fault per DIRTY page -- a trap, the handler, an
 * invalidation and an iret.  Sweeping costs one entry per page of the WHOLE
 * object, which is two orders of magnitude less each.  So faulting wins only
 * while a very small part of the object is being written, and for one being
 * repainted whole it is about a hundred times worse.
 *
 * Staying with the sweep one frame too long therefore costs one cheap sweep;
 * staying with faults one frame too long costs a full storm.  Waiting for a
 * repeat before believing the expensive evidence is what made a browser
 * oscillate -- scrolling comes in bursts, so it paid the storm at the start of
 * every one and then fell back during the idle in between.
 *
 * So: evidence that is overwhelming on its own is acted on at once, and going
 * back to faulting -- the costly mistake -- wants a long quiet spell first. */
#define DRM_GEM_DIRTY_DECISIVE_PERCENTAGE 50
#define DRM_GEM_DIRTY_BACK_TRIGGERS 15

/* Which method a brand-new tracker starts with.
 *
 * The reference starts an object of one page table's worth of entries or
 * less -- 512 -- with the sweep, and everything larger with faults, on the
 * grounds that a sweep reads the whole object while faults only touch what
 * is written.
 *
 * That reasoning holds only when very little of the object is written, and
 * it is exactly backwards for the case that matters here.  An object is
 * created because something is about to fill it, so its first frames are
 * close to fully written, and at that point faulting costs one trap per page
 * against the sweep's one entry per page -- measured at ~1.5us versus ~15ns,
 * a hundred to one.  A full-screen surface is 2250 pages, so the choice is
 * about 3.4ms of faults on the first frame against about 34us of sweep.
 *
 * So every tracker starts with the sweep, and the adaptive switch below
 * moves the ones that turn out to be sparsely written over to faults after a
 * quiet spell.  Being wrong in this direction costs one cheap sweep per
 * frame until it settles; being wrong in the other cost a storm.  This is
 * the one place the method policy deliberately departs from the reference,
 * and scratchpad/methodpolicy measures both halves of it. */
#define DRM_GEM_DIRTY_START_PAGETABLE 1

/* How many scans a fingerprint is believed for.
 *
 * The fingerprints in `fp' are a CLAIM about what the DEVICE holds, and the
 * kernel cannot see the device's copy to check it.  Anything that resets that
 * copy without the tracker hearing of it -- a surface defined over a buffer
 * that is already tracked, a host that dropped what it had -- leaves the claim
 * true for pages the device has lost, and a page nobody writes again is then
 * stale for the life of the surface.  That is a black region with the later
 * repaints striped across it: the exact picture two earlier attempts at exact
 * tracking produced, for a different reason each time.
 *
 * So the claim is given a lifetime.  Once every REFRESH_SCANS the whole object
 * is reported whatever anything else says, so nothing can stay wrong for
 * longer than that.  At sixty scans a second the cost is one full transfer per
 * second -- a sixtieth of what the whole-object fallback this replaces paid on
 * EVERY frame -- and it bounds the damage of a mistake in the tracker itself,
 * which is the part of this file that has been wrong before. */
#define DRM_GEM_DIRTY_REFRESH_SCANS 60

/* Whether the content pass is used at all, or a mapping the sweeps cannot
 * reach simply reports the whole object as it always did.
 *
 * OFF.  Three attempts have now been made to replace that whole-object answer
 * with an exact one, and all three ended with the same picture in front of the
 * user: the browser's view black, with the few pages that changed afterwards
 * striped across it.  Two of them (see the tracker's history) faulted the
 * writer's mapping and lost writes; this one asks the pages themselves, which
 * cannot lose a write -- but it answers a question the kernel cannot check,
 * "does the device already have this", and every way of being wrong about that
 * produces exactly that picture.
 *
 * So the default is the answer that has always rendered correctly.  It costs
 * bandwidth -- 60 to 100 times what the truth needs, measured -- and it is
 * what every submission paid before any of this existed.
 *
 * To turn it back on, set this to 1.  Before doing so, read
 * `content scans ... shared' in the per-second report: a buffer that backs
 * more than one coherent surface has two device copies and one set of
 * fingerprints, which is one known way to produce that picture and is now
 * refused (dirty_content_scan).  A non-zero count there means the workload
 * really does share buffers, and that this was the fault. */
#define DRM_GEM_DIRTY_CONTENT_SCAN 0

struct drm_gem_dirty {
	enum drm_gem_dirty_method method;
	unsigned int change_count;
	int ref_count;
	/* Report the whole object dirty at the next scan: a mapping went
	 * where the sweep cannot follow, or vanished with its record. */
	int full;
	/* [start, end) brackets every set bit, folded into ONE word.
	 *
	 * One word because the recorder does not hold the tracker lock, so
	 * it and the consumer must each move BOTH ends in a single step.
	 * Moving them one at a time let the two interleave: the consumer
	 * emptied `start' between the recorder's two folds, so the record
	 * left `end' pointing at its page and `start' past it -- an inverted
	 * bracket that no later pass looks inside, with the bit set and the
	 * page stale for the life of the object. */
	uint64_t brk;
	/* [wr_start, wr_end) brackets every page a fault has handed the
	 * write bit to since the last protection pass.
	 *
	 * Needed because a fault can land BETWEEN a pass and the
	 * consumption that follows it.  The reference cannot reach that
	 * state: its fault handler takes the buffer object's reservation,
	 * which the submission holds across both, so the write simply
	 * waits and lands in the next submission's record.  Nothing here
	 * serialises the two, so the page's record is consumed while the
	 * page holds the write bit -- and the bracket the next pass works
	 * from was just emptied by that same consumption, so it would
	 * never be protected again.  A page like that stops faulting, and
	 * every later write to it is lost: a stale device copy of exactly
	 * that page, for as long as the object lives.
	 *
	 * Tracking where the bit was handed out, separately from what the
	 * records bracket, closes it: the consumption may empty the record
	 * bracket, but this one still says which pages owe a pass. */
	uint64_t wr_brk;
	uint64_t bitmap_size; /* pages */
	/* One 64-bit fingerprint per page of the object's CONTENT as the
	 * device last had it, or NULL.
	 *
	 * The page tables answer "was this written"; this answers "is it
	 * different", which is the question the device actually needs and the
	 * only one that can be answered about a mapping in an address space
	 * this kernel's sweeps cannot walk.  It costs one pass over the
	 * object's bytes -- about 90us for two megabytes, against the ~6.5ms
	 * the host spends re-reading those same two megabytes when the answer
	 * is "all of it".
	 *
	 * Allocated the first time a scan would otherwise have reported the
	 * whole object, so an object that never needs it never pays for it. */
	uint64_t *fp;
	/* Scans since the whole object was last reported unconditionally.
	 * See DRM_GEM_DIRTY_REFRESH_SCANS. */
	uint32_t since_refresh;
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
/* A bracket is a (start, end) pair of page indices carried in one word so
 * both ends move together.  An object of more than 4G pages -- 16TB -- is
 * not one this driver is given, and drm_gem_dirty_add refuses it. */
#define BRK_MAKE(s, e) (((uint64_t)(uint32_t)(s) << 32) | (uint32_t)(e))
#define BRK_START(b) ((uint64_t)(uint32_t)((b) >> 32))
#define BRK_END(b) ((uint64_t)(uint32_t)(b))

/* Grow a bracket to cover `page', lock-free.  start only ever moves down and
 * end only ever moves up, so concurrent folders converge on the union. */
static void brk_fold(uint64_t *brk, uint64_t page)
{
	uint64_t old = __atomic_load_n(brk, __ATOMIC_RELAXED);
	uint64_t nw;

	do {
		uint64_t st = BRK_START(old), en = BRK_END(old);

		if (page < st)
			st = page;
		if (page + 1 > en)
			en = page + 1;
		nw = BRK_MAKE(st, en);
		if (nw == old)
			return;
	} while (!__atomic_compare_exchange_n(brk, &old, nw, 1,
					      __ATOMIC_RELAXED,
					      __ATOMIC_RELAXED));
}

static spinlock_t g_dirty_lock = SPINLOCK_INIT("drm_dirty");

/* ---- small bitmap helpers ---------------------------------------------- */

#define BITS_PER_WORD 64
/* Words in one of the two bitmaps. */
static inline uint64_t dirty_words(const struct drm_gem_dirty *d)
{
	return (d->bitmap_size + BITS_PER_WORD - 1) / BITS_PER_WORD;
}


static inline void bit_set(uint64_t *map, uint64_t n)
{
	map[n / BITS_PER_WORD] |= 1ULL << (n % BITS_PER_WORD);
}

static inline int bit_test(const uint64_t *map, uint64_t n)
{
	return (map[n / BITS_PER_WORD] >> (n % BITS_PER_WORD)) & 1;
}

/* Clearing is atomic because the RECORDER is lock-free and can be setting
 * another bit of the same word at the same moment.  A word holds 64 pages,
 * so a plain read-modify-write here reads the word, the recorder's atomic OR
 * lands, and the write-back puts the old word down again -- the record is
 * gone while the page is dirty, and the device keeps the stale copy. */
static void bits_clear_range(uint64_t *map, uint64_t start, uint64_t n)
{
	for (uint64_t i = start; i < start + n; i++)
		__atomic_fetch_and(&map[i / BITS_PER_WORD],
				   ~(1ULL << (i % BITS_PER_WORD)),
				   __ATOMIC_RELAXED);
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

/* Record one dirty page, from the clean sweep's callback or the fault.
 *
 * Without taking a lock, because this runs once per PAGE.  A full-screen
 * repaint is a couple of thousand pages per surface per frame and a browser
 * paints from several threads at once, so a lock here is taken over a hundred
 * thousand times a second from several processors -- and g_dirty_lock is ONE
 * lock for every tracked object in the system, held with interrupts disabled.
 * Uncontended that costs a few tens of nanoseconds; contended it is a cache
 * line moving between cores on every page, and the interrupts that are off
 * while spinning are the timer's and the mouse's, so the cost lands on the
 * whole machine rather than on the painting.
 *
 * Nothing here needs mutual exclusion.  The bit is set atomically, and the
 * bracket is folded with compare-and-swap: `start' only ever moves down and
 * `end' only ever moves up, so concurrent recorders converge on the union
 * whatever order they land in, which is the same answer a lock would give.
 * The reference records the same way -- a bit set and a min/max -- under the
 * page-table lock it is already holding, with no lock of its own. */
static void dirty_record_page(struct drm_gem_dirty *d, uint64_t page)
{
	if (page >= d->bitmap_size)
		return;
	__atomic_fetch_or(&d->bitmap[page / BITS_PER_WORD],
			  1ULL << (page % BITS_PER_WORD), __ATOMIC_RELAXED);

	brk_fold(&d->brk, page);
}

static void dirty_record_cb(void *arg, uint64_t page)
{
	dirty_record_page(arg, page);
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
	/* ...and the same object mapped through an exported descriptor,
	 * whose records start at byte zero.  Not walking these is what made
	 * every scan of a browser's tile answer "whole object". */
	w.ops_alt = &drm_gem_dmabuf_dirty_ops;
	w.file_base_alt = 0;
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
	/* A longer quiet spell than the other direction asks for: going back
	 * to faulting is the mistake that costs a storm, and an object that
	 * was being repainted a moment ago usually is again. */
	if (d->change_count <= DRM_GEM_DIRTY_BACK_TRIGGERS) {
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
	/* Everything that might hold the write bit: the pages recorded
	 * since the last consumption, and the pages a fault handed the bit
	 * to inside the last window between a pass and its consumption --
	 * their records are gone, the bit is not.  Emptied here, BEFORE the
	 * pass: a fault landing while it runs re-fills it and is covered by
	 * the next one. */
	uint64_t wb = __atomic_exchange_n(&d->wr_brk,
					  BRK_MAKE(d->bitmap_size, 0),
					  __ATOMIC_RELAXED);
	uint64_t rb = __atomic_load_n(&d->brk, __ATOMIC_RELAXED);
	uint64_t ws = BRK_START(wb), we = BRK_END(wb);

	start = (BRK_START(rb) < ws) ? BRK_START(rb) : ws;
	end = (BRK_END(rb) > we) ? BRK_END(rb) : we;
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

	/* Then harvest what those pages took while they HELD the bit.
	 *
	 * A page only faults on the FIRST write after it is protected; every
	 * write after that goes through silently and leaves nothing but the
	 * hardware dirty bit.  Normally the record made by that first fault
	 * still stands and covers them all -- but it does not have to: a
	 * fault landing between a pass and the consumption that follows it
	 * has its record taken away while the page keeps the bit, and from
	 * then on the page is written with no record at all.  Taking the bit
	 * back above would discard the dirty bit with it and lose every one
	 * of those writes.
	 *
	 * Runs after the protection pass, never before: that pass ends with
	 * the translations it changed gone from every processor, so nothing
	 * can still be writing through one this sweep would miss.  The same
	 * wp-then-sweep pair guards the initial pass and both method
	 * switches; this was the one place it was missing.
	 *
	 * Skipped when the pass took no write bit away: nothing in the range
	 * held one, so nothing could have been written without faulting, and
	 * there is no silent dirty bit to collect.  That is the ordinary case
	 * once a surface settles, and it keeps this from doubling the cost of
	 * every scan. */
	if (marked)
		dirty_walk(o, d, 1, start, end, foreign);

	spin_lock_irqsave(&g_dirty_lock, &fl);
	uint64_t pct = 100ULL * marked / d->bitmap_size;

	if (pct > DRM_GEM_DIRTY_PERCENTAGE)
		d->change_count++;
	else
		d->change_count = 0;
	/* Half the object faulted in one frame settles it by itself: no
	 * repeat can make faulting the cheaper answer, and waiting for one
	 * only buys two more storms. */
	if (pct < DRM_GEM_DIRTY_DECISIVE_PERCENTAGE &&
	    d->change_count <= DRM_GEM_DIRTY_CHANGE_TRIGGERS) {
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
	uint64_t keep = __atomic_load_n(&d->brk, __ATOMIC_RELAXED);

	if (BRK_START(keep) < BRK_END(keep))
		bits_set_range(d->bitmap, BRK_START(keep),
			       BRK_END(keep) - BRK_START(keep));
	spin_unlock_irqrestore(&g_dirty_lock, fl);
}

/* ---- the interface the driver uses -------------------------------------- */

/* How the scans answered, since the last read.
 *
 * The driver's per-frame report says how much of a surface the updates
 * covered, and 100% has two completely different causes: a client that
 * really did repaint the whole thing, and a tracker that could not reach
 * every mapping and so reported everything to stay correct.  The first is
 * the client's business; the second is a full-surface transfer per frame
 * that nothing asked for.  Only this tells them apart. */
static uint64_t g_scans;
static uint64_t g_scans_full;
static uint64_t g_scans_fault; /* answered by the MKWRITE method */
/* Why a scan had to report everything.  Both mean "a mapping this sweep
 * cannot reach", but they want different fixes and are counted apart:
 * `dropped' is the latch set once at setup, when records already existed
 * that the first walk could not reach; `switch' is a scan that found one
 * while running.  A dropped RECORD is deliberately not among them any
 * more -- see drm_gem_dirty_map_census. */
static uint64_t g_scans_dropped;
static uint64_t g_scans_switch;
/* What the sweep FOUND, before a full-object answer overrode it, against
 * what the object holds.
 *
 * The whole-object fallback is only waste to the extent that the truth is
 * smaller, and that is not obvious: a staging buffer the client rewrites end
 * to end every frame is genuinely 100% dirty, and tracking it perfectly would
 * transfer exactly as much.  Building a reverse map to escape the fallback is
 * a large and delicate change, so it is worth knowing which of the two this
 * is before paying for it. */
static uint64_t g_scan_found_pages;
static uint64_t g_scan_total_pages;
/* What the content pass answered instead of "the whole object". */
static uint64_t g_content_scans;
static uint64_t g_content_pages;
/* Scans the content pass REFUSED because the buffer has more than one
 * consumer, and scans that reported everything to refresh the device. */
static uint64_t g_content_shared;
static uint64_t g_scans_refresh;

/* Pages the tracker would have reported, had the fallback not overridden it. */
static uint64_t dirty_bits_population(const struct drm_gem_dirty *d)
{
	uint64_t n = 0;

	/* Counted by hand: __builtin_popcountll becomes a libgcc call in a
	 * freestanding build compiled without the POPCNT instruction, and
	 * this kernel links no libgcc.  The SWAR fold below is branch-free
	 * and runs over eight words for a two-megabyte object. */
	for (uint64_t w = 0; w < dirty_words(d); w++) {
		uint64_t v = d->bitmap[w];

		v = v - ((v >> 1) & 0x5555555555555555ULL);
		v = (v & 0x3333333333333333ULL) +
		    ((v >> 2) & 0x3333333333333333ULL);
		v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
		n += (v * 0x0101010101010101ULL) >> 56;
	}
	return n;
}

void drm_gem_dirty_scan_found(uint64_t *found_pages, uint64_t *total_pages)
{
	if (found_pages)
		*found_pages = __atomic_exchange_n(&g_scan_found_pages, 0,
						   __ATOMIC_RELAXED);
	if (total_pages)
		*total_pages = __atomic_exchange_n(&g_scan_total_pages, 0,
						   __ATOMIC_RELAXED);
}

void drm_gem_dirty_scan_content(uint64_t *scans, uint64_t *pages,
				uint64_t *shared, uint64_t *refresh)
{
	if (scans)
		*scans = __atomic_exchange_n(&g_content_scans, 0,
					     __ATOMIC_RELAXED);
	if (pages)
		*pages = __atomic_exchange_n(&g_content_pages, 0,
					     __ATOMIC_RELAXED);
	if (shared)
		*shared = __atomic_exchange_n(&g_content_shared, 0,
					      __ATOMIC_RELAXED);
	if (refresh)
		*refresh = __atomic_exchange_n(&g_scans_refresh, 0,
					       __ATOMIC_RELAXED);
}

void drm_gem_dirty_scan_why(uint64_t *dropped, uint64_t *switched)
{
	if (dropped)
		*dropped = __atomic_exchange_n(&g_scans_dropped, 0,
					       __ATOMIC_RELAXED);
	if (switched)
		*switched = __atomic_exchange_n(&g_scans_switch, 0,
						__ATOMIC_RELAXED);
}

void drm_gem_dirty_scan_stats(uint64_t *scans, uint64_t *full, uint64_t *fault)
{
	if (scans)
		*scans = __atomic_exchange_n(&g_scans, 0, __ATOMIC_RELAXED);
	if (full)
		*full = __atomic_exchange_n(&g_scans_full, 0, __ATOMIC_RELAXED);
	if (fault)
		*fault = __atomic_exchange_n(&g_scans_fault, 0, __ATOMIC_RELAXED);
}

/* A page's content, in one word.
 *
 * FNV-1a over the page as 64-bit words: one exclusive-or and one multiply per
 * eight bytes, no table, no wide types, and nothing the freestanding build
 * turns into a libgcc call.  Two pages that differ are called equal only on a
 * 64-bit collision, and the cost of one is a single stale page until its next
 * change -- against a corrupt frame if the answer were wrong the other way. */
static uint64_t dirty_page_fingerprint(const uint64_t *p)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	for (uint32_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	/* A page of zeroes must not fingerprint to the initial value, or a
	 * freshly allocated table would call it unchanged. */
	return h ^ (h >> 32);
}

/* Report the pages whose CONTENT changed since the device last saw them.
 *
 * This is what replaces reporting the whole object when a mapping exists that
 * the sweeps cannot reach.  It asks nothing of the page tables -- no write
 * bit taken, no fault, no invalidation, and no address space touched but the
 * kernel's own view of the pages -- so it is exact whoever wrote them and
 * from wherever.  Two attempts to answer this with page-table tracking
 * instead both ended in lost writes and a hung machine; the note in
 * dirty-selfarm says why that is not fixable.
 *
 * Ordering that matters: the fingerprint is taken BEFORE the device reads the
 * page.  A write landing between the two leaves the device with content newer
 * than the fingerprint, so the next scan sees a difference and sends it
 * again -- wasteful and safe.  Taken after, the fingerprint would match
 * content the device never got, and the page would stay stale for ever.
 *
 * Returns 0 if it could answer, -1 if the caller must fall back to reporting
 * everything. */
static int dirty_content_scan(struct drm_gem_object *o,
			      struct drm_gem_dirty *d, int shared)
{
	uint64_t n = d->bitmap_size;

	if (!o->pages)
		return -1; /* nothing here can read the content */

	/* One set of fingerprints describes ONE device copy, and the tracker
	 * belongs to the BUFFER while the copy belongs to the SURFACE.  A
	 * buffer that backs two coherent surfaces therefore has two device
	 * copies and one set of fingerprints, and the second surface is the
	 * one that suffers: the scan run for it finds every page already
	 * accounted for by the scan run for the first, reports nothing, and
	 * the surface keeps whatever its copy was born with -- black, with
	 * only the pages that change between the two scans ever arriving.
	 *
	 * The whole-object answer this replaces did not have that problem:
	 * every scan reported everything, so each surface's own scan handed
	 * it the whole buffer.  So that is what a shared buffer keeps.  It
	 * costs the bandwidth it always cost, and it is right.
	 *
	 * (Both surfaces' windows are the whole buffer today -- backup_offset
	 * is always zero -- so they overlap completely; the transfer window
	 * cannot separate them either.) */
	if (shared)
		return -1;
	if (!d->fp) {
		/* Zeroed: every page differs on the first pass, which is the
		 * whole-object report this replaces -- once. */
		d->fp = kalloc(n * sizeof(uint64_t));
		if (!d->fp)
			return -1;
		mm_memset(d->fp, 0, n * sizeof(uint64_t));
	}
	for (uint64_t i = 0; i < n; i++) {
		const uint64_t *pg = drm_gem_page_virt(o, (uint32_t)i);
		uint64_t h;

		if (!pg)
			continue;
		h = dirty_page_fingerprint(pg);
		if (h == d->fp[i])
			continue;
		d->fp[i] = h;
		dirty_record_page(d, i);
		__atomic_fetch_add(&g_content_pages, 1, __ATOMIC_RELAXED);
	}
	__atomic_fetch_add(&g_content_scans, 1, __ATOMIC_RELAXED);
	return 0;
}

void drm_gem_dirty_scan(struct drm_gem_object *o)
{
	struct drm_gem_dirty *d;
	int foreign = 0;   /* records this sweep could not reach */
	int report_all = 0;
	int refresh = 0;   /* the periodic unconditional full report */
	int shared = 0;    /* more than one consumer of this buffer */
	uint64_t fl;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	d = o->dirty;
	if (d && d->full) {
		d->full = 0;
		report_all = 1;
		__atomic_fetch_add(&g_scans_dropped, 1, __ATOMIC_RELAXED);
	}
	spin_unlock_irqrestore(&g_dirty_lock, fl);
	if (!d)
		return;

	__atomic_fetch_add(&g_scans, 1, __ATOMIC_RELAXED);
	if (d->method == DRM_GEM_DIRTY_PAGETABLE) {
		dirty_scan_pagetable(o, d, &foreign);
	} else {
		__atomic_fetch_add(&g_scans_fault, 1, __ATOMIC_RELAXED);
		dirty_scan_mkwrite(o, d, &foreign);
	}

	/* Sampled here: after the sweep has said what it found, before the
	 * override below replaces it with everything. */
	spin_lock_irqsave(&g_dirty_lock, &fl);
	__atomic_fetch_add(&g_scan_found_pages, dirty_bits_population(d),
			   __ATOMIC_RELAXED);
	__atomic_fetch_add(&g_scan_total_pages, d->bitmap_size,
			   __ATOMIC_RELAXED);
	spin_unlock_irqrestore(&g_dirty_lock, fl);

	if (foreign) {
		/* Mappings exist that the sweep could not reach, or one
		 * vanished with its record: every page might have been
		 * written, so every page is reported.  Costly and correct;
		 * see the header comment. */
		report_all = 1;
		__atomic_fetch_add(&g_scans_switch, 1, __ATOMIC_RELAXED);
	}

	/* The device's copy is not observable from here, so no answer this
	 * file gives can be checked against it.  Once every REFRESH_SCANS the
	 * whole object goes out regardless -- of the fingerprints, of the
	 * sweeps, of anything -- so a page the device lost for a reason the
	 * tracker never heard about is stale for a second and not for ever.
	 * See DRM_GEM_DIRTY_REFRESH_SCANS. */
	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (++d->since_refresh >= DRM_GEM_DIRTY_REFRESH_SCANS) {
		d->since_refresh = 0;
		refresh = 1;
	}
	/* Counted whether or not the content pass runs, because it is the
	 * measurement that says whether the pass could ever be trusted here. */
	shared = d->ref_count > 1;
	spin_unlock_irqrestore(&g_dirty_lock, fl);
	if (refresh) {
		report_all = 1;
		__atomic_fetch_add(&g_scans_refresh, 1, __ATOMIC_RELAXED);
	}
	if (shared)
		__atomic_fetch_add(&g_content_shared, 1, __ATOMIC_RELAXED);

	/* Whatever the sweeps could not answer for, the content answers --
	 * outside the lock, because it reads every page of the object and the
	 * lock is the one every tracked object in the system shares.  The
	 * recorder it calls is lock-free by design, and the object cannot go
	 * away underneath it: the submission this scan runs for holds a
	 * reference on the resource that holds the tracking reference. */
	if (DRM_GEM_DIRTY_CONTENT_SCAN && report_all && !refresh &&
	    dirty_content_scan(o, d, shared) == 0)
		report_all = 0;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (report_all) {
		__atomic_fetch_add(&g_scans_full, 1, __ATOMIC_RELAXED);
		bits_set_range(d->bitmap, 0, d->bitmap_size);
		__atomic_store_n(&d->brk, BRK_MAKE(0, d->bitmap_size),
				 __ATOMIC_RELAXED);
	}
	spin_unlock_irqrestore(&g_dirty_lock, fl);
}

/* Put back the bracket for whatever a windowed transfer did not take.
 *
 * The bracket is emptied BEFORE the walk, not narrowed after it.  Narrowing
 * after looks equivalent and is not: the recorder sets its bit and widens the
 * bracket as two separate steps, so one can set a bit that the walk has
 * already passed, find the bracket still covering it and leave it alone, and
 * then have this overwrite the bracket with a value that excludes it.  The
 * bit stays set outside the bracket and nothing looks at it again.  The
 * reference is free of that because its recorder cannot run at the same time
 * as its consumer -- both hold the buffer's reservation -- and it narrows by
 * arithmetic instead.
 *
 * So the leftovers are folded back in, never assigned: folding only ever
 * WIDENS, which merges correctly with whatever a recorder added meanwhile. */
static void dirty_rebracket_leftovers(struct drm_gem_dirty *d, uint64_t lo,
				      uint64_t hi)
{
	uint64_t cur = lo;
	uint64_t ns = 0, ne = 0;
	int any = 0;

	while (cur < hi) {
		uint64_t first = bits_next_set(d->bitmap, hi, cur);

		if (first >= hi)
			break;
		uint64_t last = bits_next_clear(d->bitmap, hi, first + 1);

		if (!any) {
			ns = first;
			any = 1;
		}
		ne = last;
		cur = last + 1;
	}
	if (!any)
		return;
	/* The two ends are enough: the bracket is a span, so covering the
	 * first and the last set page covers everything between them. */
	brk_fold(&d->brk, ns);
	brk_fold(&d->brk, ne - 1);
}

/* Hand the driver the pages written inside ONE WINDOW of the object, and
 * clear only those.
 *
 * The window matters because a buffer object can back more than one
 * resource -- a surface is created against whatever buffer handle the client
 * names, and the client's allocator puts several in one -- while the dirty
 * tracking belongs to the BUFFER.  Consuming the whole bitmap therefore hands
 * the first resource to ask both its own pages and its neighbours', at
 * offsets that mean nothing to it, and CLEARS them: the neighbour is never
 * told its pages changed and the device keeps showing what it had.  That does
 * not repaint on its own, because nothing writes those pages again until the
 * content itself is redrawn.
 *
 * [first_page, last_page) is in pages of the OBJECT; the callback is given
 * object-relative page numbers and the caller subtracts its own base. */
void drm_gem_dirty_transfer(struct drm_gem_object *o, uint64_t first_page,
			    uint64_t last_page,
			    void (*cb)(void *arg, uint64_t first,
				       uint64_t last),
			    void *arg)
{
	struct drm_gem_dirty *d;
	uint64_t fl;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	d = o->dirty;
	if (!d) {
		spin_unlock_irqrestore(&g_dirty_lock, fl);
		return;
	}
	if (last_page > d->bitmap_size)
		last_page = d->bitmap_size;

	/* Empty the bracket before reading the bitmap; the leftovers go back
	 * afterwards.  See dirty_rebracket_leftovers() for why this order and
	 * not the other. */
	uint64_t b = __atomic_exchange_n(&d->brk,
					 BRK_MAKE(d->bitmap_size, 0),
					 __ATOMIC_RELAXED);
	uint64_t lo = BRK_START(b), hi = BRK_END(b);
	uint64_t cur = lo;
	uint64_t limit = hi;

	/* Only where the bracket and the window overlap. */
	if (cur < first_page)
		cur = first_page;
	if (limit > last_page)
		limit = last_page;

	/* Runs of set bits become calls; the callback must not sleep (it
	 * runs under the tracker lock and only shapes driver structures). */
	while (cur < limit) {
		uint64_t first = bits_next_set(d->bitmap, limit, cur);

		if (first >= limit)
			break;
		uint64_t last = bits_next_clear(d->bitmap, limit, first + 1);

		bits_clear_range(d->bitmap, first, last - first);
		cb(arg, first, last);
		cur = last + 1;
	}
	/* Everything in the old bracket that this window did not cover -- the
	 * other resources' pages -- is still set and has to stay bracketed. */
	dirty_rebracket_leftovers(d, lo, hi);
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
		struct drm_gem_dirty *ex = o->dirty;
		uint64_t *fp;

		ex->ref_count++;
		/* A second consumer of this buffer, whose device copy holds
		 * NOTHING while the fingerprints describe what the first one
		 * received.  Forget them and report everything at the next
		 * scan, or the new surface is told about only the pages that
		 * change from here on and shows its empty copy for the rest.
		 *
		 * Zeroed rather than freed: a scan may be reading the array
		 * outside the lock, and a fingerprint zeroed under a reader
		 * can only make it report a page again.  The reference the
		 * count above just took is what keeps the array alive. */
		ex->full = 1;
		ex->since_refresh = 0;
		fp = ex->fp;
		spin_unlock_irqrestore(&g_dirty_lock, fl);
		if (fp)
			mm_memset(fp, 0, ex->bitmap_size * sizeof(uint64_t));
		return 0;
	}
	spin_unlock_irqrestore(&g_dirty_lock, fl);

	if (!pages || pages > 0xffffffffull)
		return -EINVAL; /* a bracket carries page indices in 32 bits */
	size_t bytes = sizeof(*d) +
		       ((pages + BITS_PER_WORD - 1) / BITS_PER_WORD) *
			       sizeof(uint64_t);

	d = kalloc(bytes);
	if (!d)
		return -ENOMEM;
	mm_memset(d, 0, bytes);
	d->bitmap_size = pages;
	d->brk = BRK_MAKE(pages, 0);
	d->wr_brk = BRK_MAKE(pages, 0);
	d->ref_count = 1;
	d->method = DRM_GEM_DIRTY_START_PAGETABLE ? DRM_GEM_DIRTY_PAGETABLE :
						    DRM_GEM_DIRTY_MKWRITE;

	spin_lock_irqsave(&g_dirty_lock, &fl);
	if (o->dirty) {
		/* Two creators raced; the first one's tracker stands -- and
		 * this is a second consumer of it, so its fingerprints mean
		 * nothing for the copy this one is about to fill. */
		struct drm_gem_dirty *ex = o->dirty;
		uint64_t *fp;

		ex->ref_count++;
		ex->full = 1;
		ex->since_refresh = 0;
		fp = ex->fp;
		spin_unlock_irqrestore(&g_dirty_lock, fl);
		if (fp)
			mm_memset(fp, 0, ex->bitmap_size * sizeof(uint64_t));
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
	} else {
		/* Sweeping leaves the pages writable, so there is nothing to
		 * protect -- but the collecting pass still has to run.  It
		 * does the same two jobs it does above: it picks up writes
		 * older than the tracker, which still owe the device one
		 * report, and it counts the mappings, which is what tells us
		 * whether any of them are out of this sweep's reach. */
		dirty_walk(o, d, 1, 0, pages, &foreign);
	}
	if (foreign) {
		spin_lock_irqsave(&g_dirty_lock, &fl);
		d->full = 1;
		spin_unlock_irqrestore(&g_dirty_lock, fl);
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
	if (d) {
		if (d->fp)
			kfree(d->fp);
		kfree(d);
	}
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
	if (o->dirty && o->dirty->method == DRM_GEM_DIRTY_MKWRITE) {
		struct drm_gem_dirty *d = o->dirty;

		dirty_record_page(d, page);
		/* The bit is about to be handed back; remember where, so the
		 * next pass takes it away again even if this record has been
		 * consumed by then.  Folded the same lock-free way. */
		brk_fold(&d->wr_brk, page);
	}
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
	.map_census = drm_gem_dirty_map_census,
};

/* ---- mapping-record census --------------------------------------------- */

/* Called by the mmap get/put wrappers as region records referencing the
 * object come and go: the initial mapping, splits, forked copies, and a
 * protection change either way.  The count is the ONE thing kept here: it is
 * what a sweep compares its walk against, so that a record it could not reach
 * is recognised.  Only WRITABLE records are counted -- see
 * mm_dirty_ops.map_census. */
void drm_gem_dirty_map_census(void *obj, int add)
{
	struct drm_gem_object *o = obj;

	__atomic_add_fetch(&o->map_records, add ? 1 : -1, __ATOMIC_RELAXED);
	/* A dropped record does NOT condemn the object.
	 *
	 * It used to: a record going away might take dirty bits the sweep had
	 * not harvested, so the next scan reported everything.  That is a
	 * hundred times the traffic the truth needs, and it fires constantly
	 * -- a compositor that maps a buffer, reads it and unmaps it once per
	 * frame re-condemned the object on every frame it displayed, so the
	 * tracker never reported anything but "all of it" for the life of the
	 * surface.
	 *
	 * It is also unnecessary, because nothing is lost any more.  Every
	 * path that DISCARDS a device mapping's entries hands their dirty
	 * bits over first (mm_region_harvest_dirty: munmap for the range it
	 * retires, the exit teardown for the whole address space), and the
	 * paths that merely retire a record -- a merge absorbing it, a
	 * protection change -- discard no entry at all, so there was never
	 * anything to lose there.
	 *
	 * What still reports the whole object is a mapping the sweep cannot
	 * REACH, which is a different condition, counted separately, and
	 * decided per scan rather than latched. */
}
