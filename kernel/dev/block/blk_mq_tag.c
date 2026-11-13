// LikeOS - multi-queue block layer: tags (scalable bitmap allocator),
// the per-tag driver data, and the busy-tag iterators.
//
// ---- The scalable bitmap (struct blk_sbitmap_queue) -------------------------
// A depth of N tags is a bitmap split into words of 2^shift bits (shift is
// chosen so that even a small depth is spread over at least four words):
// allocators on different CPUs start in different words -- each CPU keeps
// an allocation hint -- and touch different cache lines.  A freed bit is
// not cleared in the word at once: it is set in the word's `cleared'
// companion and folded in (one exchange for every bit freed meanwhile) by
// the next allocator that finds the word full.  With round-robin
// allocation (BLK_MQ_F_TAG_RR) the hint always moves forward and a word is
// searched from the hint without wrapping, so tags are handed out in
// order.
//
// Waiters for a free tag sleep on one of BLK_SBQ_WAIT_QUEUES wait queues
// (chosen round robin, so a wake-up never has to walk every sleeper).  A
// freed tag wakes nobody by itself: every wake_batch frees (at most 8, at
// least 1, depth / 8 normally) one wait queue is woken.  A sleeper only
// sleeps after it found every tag in use, so at least `depth' frees -- far
// more than a batch -- are on their way.
//
// ---- Tags of a hardware queue (struct blk_mq_tags) ---------------------------
// Tags 0 .. nr_reserved_tags-1 are the reserved pool (driver-internal
// commands that must never wait behind ordinary I/O, such as the NVMe
// asynchronous event requests); the normal pool follows.  tags->rqs[tag]
// is the request holding a tag; tags->pdu[tag] the driver's per-command
// data (cmd_size bytes, allocated once, set up by ops->init_request),
// preceded by a hidden header that leads back to the request.
//
// ---- Shared tags --------------------------------------------------------------
// BLK_MQ_F_TAG_HCTX_SHARED: one struct blk_mq_tags serves every hardware
// queue of the set.  BLK_MQ_F_TAG_QUEUE_SHARED: several request queues use
// the tags of one hardware queue (the namespaces of one NVMe controller);
// each active queue then gets a fair share, max(depth / active, 4), as
// the established allocator does, and a dispatch that found no tag is
// re-run by the next free of a tag (tags->restart_head).
//
// ---- Request lifetime against the iterators ---------------------------------
// The request memory belongs to its submitter (often its stack).  An
// iterator (timeout scan, tagset_busy_iter) takes the request with
// tags->lock held and a reference on rq->mq_ref -- only while
// tags->rqs[tag] still names it -- and the end of a request clears
// tags->rqs[tag] under the same lock only after the last reference went.
// So an iterator never touches a request that its submitter may already
// have freed, and the submitter learns of the end only after every
// iterator let go.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/smp.h>
#include <kernel/hal/lapic.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* Wake-up batch bound. */
#define BLK_SBQ_WAKE_BATCH 8
/* A sleeper re-checks at least this often (a safety net against a wake
 * batch that never fills: a depth smaller than the batch, a shrink). */
#define BLK_SBQ_WAIT_RECHECK_MS 100
/* Fair share floor of a shared tag map. */
#define BLK_MQ_TAG_SHARE_MIN 4

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

static inline uint32_t blk_tag_cpu(void)
{
	int cpu = blk_this_cpu();

	return (cpu < 0 || cpu >= MAX_CPUS) ? 0u : (uint32_t)cpu;
}

/* A 64-byte aligned, zeroed allocation; *raw gets what kfree takes. */
static void *blk_tag_alloc_aligned(size_t bytes, void **raw)
{
	uint8_t *p = (uint8_t *)kcalloc(1, bytes + 64);

	*raw = p;
	if (!p)
		return NULL;
	return (void *)(((uintptr_t)p + 63) & ~(uintptr_t)63);
}

/* A spread of the per-CPU starting hints (Knuth's multiplicative hash),
 * so CPUs do not all start in the first word. */
static inline unsigned int blk_sbq_hash_hint(uint32_t cpu, unsigned int depth)
{
	return depth ? (unsigned int)(((uint64_t)(cpu + 1) * 2654435761u) %
				      depth) :
		       0;
}

/* ==========================================================================
 * The scalable bitmap
 * ========================================================================== */

static unsigned int blk_sbq_calc_shift(unsigned int depth)
{
	unsigned int shift = 6; /* 64 bits per word */

	/* At least four words when the depth allows it: allocators spread
	 * over cache lines instead of all fighting over one word. */
	if (depth >= 4) {
		while ((4u << shift) > depth)
			shift--;
	}
	return shift;
}

static unsigned int blk_sbq_calc_wake_batch(unsigned int depth)
{
	unsigned int b = depth / BLK_SBQ_WAIT_QUEUES;

	if (b < 1)
		b = 1;
	if (b > BLK_SBQ_WAKE_BATCH)
		b = BLK_SBQ_WAKE_BATCH;
	return b;
}

/* Bits of word `index'. */
static inline unsigned int blk_sbq_word_depth(const struct blk_sbitmap_queue *sbq,
					      unsigned int index)
{
	unsigned int per = 1u << sbq->shift;

	if (index == sbq->map_nr - 1)
		return sbq->depth - index * per;
	return per;
}

int blk_sbq_init(struct blk_sbitmap_queue *sbq, unsigned int depth,
		 int round_robin, const char *name)
{
	BUG_ON(sbq == NULL);
	mm_memset(sbq, 0, sizeof(*sbq));
	sbq->depth = depth;
	sbq->shift = blk_sbq_calc_shift(depth);
	sbq->map_nr = depth ? (depth + (1u << sbq->shift) - 1) >> sbq->shift : 0;
	sbq->round_robin = round_robin;
	sbq->wake_batch = blk_sbq_calc_wake_batch(depth);
	sbq->min_shallow_depth = 0xFFFFFFFFu;
	for (int i = 0; i < BLK_SBQ_WAIT_QUEUES; i++)
		wq_head_init(&sbq->ws[i], name ? name : "blk_sbq");
	if (sbq->map_nr) {
		sbq->map = (struct blk_sbitmap_word *)blk_tag_alloc_aligned(
			(size_t)sbq->map_nr * sizeof(struct blk_sbitmap_word),
			&sbq->map_raw);
		if (!sbq->map)
			return -ENOMEM;
	}
	for (uint32_t c = 0; c < MAX_CPUS; c++)
		sbq->alloc_hint[c] = blk_sbq_hash_hint(c, depth);
	return 0;
}

void blk_sbq_free(struct blk_sbitmap_queue *sbq)
{
	if (!sbq)
		return;
	if (sbq->map_raw)
		kfree(sbq->map_raw);
	sbq->map_raw = NULL;
	sbq->map = NULL;
	sbq->map_nr = 0;
	sbq->depth = 0;
}

/* Fold the deferred frees of a word into it.  1 when there were any. */
static int blk_sbq_deferred_clear(struct blk_sbitmap_word *w)
{
	uint64_t mask;

	if (!__atomic_load_n(&w->cleared, __ATOMIC_RELAXED))
		return 0;
	mask = __atomic_exchange_n(&w->cleared, 0, __ATOMIC_ACQUIRE);
	(void)__atomic_fetch_and(&w->word, ~mask, __ATOMIC_RELEASE);
	return 1;
}

/* A free bit of one word from `hint' (wrapping to 0 when `wrap'), set
 * atomically.  -1 when the word is full. */
static int blk_sbq_find_set_bit(struct blk_sbitmap_word *w, unsigned int depth,
				unsigned int hint, int wrap)
{
	if (hint >= depth)
		hint = 0;
	for (;;) {
		uint64_t cur = __atomic_load_n(&w->word, __ATOMIC_RELAXED);
		uint64_t mask = depth >= 64 ? ~0ULL : ((1ULL << depth) - 1);
		uint64_t free_bits = ~cur & mask & (~0ULL << hint);
		unsigned int nr;

		if (!free_bits) {
			if (hint && wrap) {
				hint = 0;
				continue;
			}
			return -1;
		}
		nr = (unsigned int)__builtin_ctzll(free_bits);
		if (!(__atomic_fetch_or(&w->word, 1ULL << nr, __ATOMIC_ACQUIRE) &
		      (1ULL << nr)))
			return (int)nr;
		/* Lost the race for that bit: look further. */
		hint = nr + 1;
		if (hint >= depth - 1)
			hint = 0;
	}
}

static int blk_sbq_get_word(struct blk_sbitmap_queue *sbq, unsigned int index,
			    unsigned int hint, int wrap)
{
	struct blk_sbitmap_word *w = &sbq->map[index];
	unsigned int depth = blk_sbq_word_depth(sbq, index);
	int nr;

	for (;;) {
		nr = blk_sbq_find_set_bit(w, depth, hint, wrap);
		if (nr != -1)
			return nr;
		if (!blk_sbq_deferred_clear(w))
			return -1;
	}
}

/* Take a free bit.  -1 when the map is full.  Any context. */
int blk_sbq_get(struct blk_sbitmap_queue *sbq)
{
	uint32_t cpu = blk_tag_cpu();
	unsigned int hint, index, bit;
	int nr = -1;

	if (!sbq->depth)
		return -1;
	hint = __atomic_load_n(&sbq->alloc_hint[cpu], __ATOMIC_RELAXED);
	if (hint >= sbq->depth) {
		hint = blk_sbq_hash_hint(cpu + (uint32_t)sbq->depth, sbq->depth);
		__atomic_store_n(&sbq->alloc_hint[cpu], hint, __ATOMIC_RELAXED);
	}
	index = hint >> sbq->shift;
	/* Round robin starts at the hint inside its word and does not wrap
	 * there; otherwise the hint only chooses the word. */
	bit = sbq->round_robin ? (hint & ((1u << sbq->shift) - 1)) : 0;
	for (unsigned int i = 0; i < sbq->map_nr; i++) {
		nr = blk_sbq_get_word(sbq, index, bit, !sbq->round_robin);
		if (nr != -1) {
			nr += (int)(index << sbq->shift);
			break;
		}
		bit = 0;
		if (++index >= sbq->map_nr)
			index = 0;
	}
	/* The hint after the attempt (the established update rule). */
	if (nr == -1) {
		__atomic_store_n(&sbq->alloc_hint[cpu], 0, __ATOMIC_RELAXED);
	} else if ((unsigned int)nr == hint || sbq->round_robin) {
		unsigned int next = (unsigned int)nr + 1;

		if (next >= sbq->depth - 1)
			next = 0;
		__atomic_store_n(&sbq->alloc_hint[cpu], next, __ATOMIC_RELAXED);
	}
	return nr;
}

/* Wake one wait queue per wake_batch frees. */
static void blk_sbq_wake_up(struct blk_sbitmap_queue *sbq, unsigned int nr)
{
	uint32_t batch = __atomic_load_n(&sbq->wake_batch, __ATOMIC_RELAXED);
	uint32_t wakeups;

	/* Pairs with the waiter's increment of ws_active before its last
	 * look at the map. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&sbq->ws_active, __ATOMIC_SEQ_CST))
		return;
	(void)__atomic_add_fetch(&sbq->completion_cnt, nr, __ATOMIC_SEQ_CST);
	wakeups = __atomic_load_n(&sbq->wakeup_cnt, __ATOMIC_RELAXED);
	do {
		if (__atomic_load_n(&sbq->completion_cnt, __ATOMIC_RELAXED) -
			    wakeups <
		    batch)
			return;
	} while (!__atomic_compare_exchange_n(&sbq->wakeup_cnt, &wakeups,
					      wakeups + batch, 0,
					      __ATOMIC_SEQ_CST,
					      __ATOMIC_RELAXED));
	{
		unsigned int idx = __atomic_load_n(&sbq->wake_index,
						   __ATOMIC_RELAXED);

		for (int i = 0; i < BLK_SBQ_WAIT_QUEUES; i++) {
			struct wait_queue_head *ws =
				&sbq->ws[idx % BLK_SBQ_WAIT_QUEUES];

			idx++;
			if (__atomic_load_n(&ws->first, __ATOMIC_ACQUIRE)) {
				wake_up(ws);
				break;
			}
		}
		__atomic_store_n(&sbq->wake_index, idx % BLK_SBQ_WAIT_QUEUES,
				 __ATOMIC_RELAXED);
	}
}

/* Wake every sleeper (resize, teardown). */
void blk_sbq_wake_all(struct blk_sbitmap_queue *sbq)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&sbq->ws_active, __ATOMIC_SEQ_CST))
		return;
	for (int i = 0; i < BLK_SBQ_WAIT_QUEUES; i++)
		wake_up(&sbq->ws[i]);
}

/* Give bit `nr' back (deferred), wake per batch, and -- unless round
 * robin -- make it this CPU's next hint (it is hot in this cache). */
void blk_sbq_clear(struct blk_sbitmap_queue *sbq, unsigned int nr, uint32_t cpu)
{
	unsigned int index;

	BUG_ON(nr >= sbq->depth);
	index = nr >> sbq->shift;
	(void)__atomic_fetch_or(&sbq->map[index].cleared,
				1ULL << (nr & ((1u << sbq->shift) - 1)),
				__ATOMIC_RELEASE);
	blk_sbq_wake_up(sbq, 1);
	if (!sbq->round_robin && cpu < MAX_CPUS)
		__atomic_store_n(&sbq->alloc_hint[cpu], nr, __ATOMIC_RELAXED);
}

/* Bits in use (deferred frees excluded). */
unsigned int blk_sbq_weight(const struct blk_sbitmap_queue *sbq)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < sbq->map_nr; i++) {
		uint64_t w = __atomic_load_n(&sbq->map[i].word, __ATOMIC_RELAXED);
		uint64_t c = __atomic_load_n(&sbq->map[i].cleared,
					     __ATOMIC_RELAXED);

		n += blk_popcount64(w & ~c);
	}
	return n;
}

static int blk_sbq_test_bit(const struct blk_sbitmap_queue *sbq,
			    unsigned int nr)
{
	unsigned int index = nr >> sbq->shift;
	uint64_t bit = 1ULL << (nr & ((1u << sbq->shift) - 1));
	uint64_t w = __atomic_load_n(&sbq->map[index].word, __ATOMIC_ACQUIRE);
	uint64_t c = __atomic_load_n(&sbq->map[index].cleared,
				     __ATOMIC_ACQUIRE);

	return (w & bit) && !(c & bit);
}

/* ==========================================================================
 * Tags
 * ========================================================================== */

static inline struct blk_mq_pdu_hdr *blk_mq_pdu_hdr_of(void *pdu)
{
	return (struct blk_mq_pdu_hdr *)((uint8_t *)pdu -
					 sizeof(struct blk_mq_pdu_hdr));
}

void blk_mq_free_rqs(struct blk_mq_tag_set *set, struct blk_mq_tags *tags)
{
	if (!tags || !tags->pdu)
		return;
	if (set && set->ops && set->ops->exit_request) {
		for (unsigned int i = 0; i < tags->nr_tags; i++)
			set->ops->exit_request(set, tags->pdu[i], tags->hctx_idx,
					       i);
	}
}

void blk_mq_free_tags(struct blk_mq_tags *tags)
{
	if (!tags)
		return;
	blk_sbq_free(&tags->bitmap_tags);
	blk_sbq_free(&tags->breserved_tags);
	if (tags->rqs)
		kfree((void *)tags->rqs);
	if (tags->pdu)
		kfree(tags->pdu);
	if (tags->pdu_raw)
		kfree(tags->pdu_raw);
	kfree(tags);
}

/* Tags, request table and PDUs of one hardware queue (or the shared
 * map: hctx_idx BLK_MQ_NO_HCTX_IDX), init_request run for each tag.
 * NULL on -ENOMEM or an init_request failure.  May sleep. */
struct blk_mq_tags *blk_mq_alloc_map_and_rqs(struct blk_mq_tag_set *set,
					     unsigned int hctx_idx,
					     unsigned int depth)
{
	struct blk_mq_tags *tags;
	unsigned int normal;
	size_t cmd;

	BUG_ON(set == NULL);
	if (depth == 0 || depth > BLK_MQ_MAX_DEPTH ||
	    set->reserved_tags >= depth)
		return NULL;
	tags = (struct blk_mq_tags *)kcalloc(1, sizeof(*tags));
	if (!tags)
		return NULL;
	spinlock_init(&tags->lock, "blk_mq_tags");
	tags->set = set;
	tags->hctx_idx = hctx_idx;
	tags->nr_tags = depth;
	tags->nr_reserved_tags = set->reserved_tags;
	refcount_set(&tags->ref, 1);
	normal = depth - set->reserved_tags;
	if (blk_sbq_init(&tags->bitmap_tags, normal,
			 (set->flags & BLK_MQ_F_TAG_RR) != 0, "blk_mq_tag_wait") ||
	    blk_sbq_init(&tags->breserved_tags, set->reserved_tags,
			 (set->flags & BLK_MQ_F_TAG_RR) != 0,
			 "blk_mq_rtag_wait"))
		goto fail;
	tags->rqs = (struct blk_request *volatile *)kcalloc(
		depth, sizeof(struct blk_request *));
	tags->pdu = (void **)kcalloc(depth, sizeof(void *));
	if (!tags->rqs || !tags->pdu)
		goto fail;
	cmd = ((size_t)set->cmd_size + 63) & ~(size_t)63;
	tags->pdu_stride = (unsigned int)(sizeof(struct blk_mq_pdu_hdr) + cmd);
	tags->pdu_mem = blk_tag_alloc_aligned((size_t)depth * tags->pdu_stride,
					      &tags->pdu_raw);
	if (!tags->pdu_mem)
		goto fail;
	for (unsigned int i = 0; i < depth; i++) {
		uint8_t *base = (uint8_t *)tags->pdu_mem +
				(size_t)i * tags->pdu_stride;
		struct blk_mq_pdu_hdr *h = (struct blk_mq_pdu_hdr *)base;

		h->rq = NULL;
		h->tag = i;
		h->magic = BLK_MQ_PDU_MAGIC;
		h->tags = tags;
		tags->pdu[i] = base + sizeof(struct blk_mq_pdu_hdr);
	}
	if (set->ops && set->ops->init_request) {
		for (unsigned int i = 0; i < depth; i++) {
			int rc = set->ops->init_request(set, tags->pdu[i],
							hctx_idx, i);

			if (rc) {
				/* Undo the ones that worked. */
				if (set->ops->exit_request)
					for (unsigned int j = 0; j < i; j++)
						set->ops->exit_request(
							set, tags->pdu[j],
							hctx_idx, j);
				goto fail;
			}
		}
	}
	return tags;

fail:
	blk_mq_free_tags(tags);
	return NULL;
}

struct blk_request *blk_mq_tag_to_rq(struct blk_mq_tags *tags, unsigned int tag)
{
	if (!tags || tag >= tags->nr_tags)
		return NULL;
	return __atomic_load_n(&tags->rqs[tag], __ATOMIC_ACQUIRE);
}

uint32_t blk_mq_unique_tag(struct blk_request *rq)
{
	BUG_ON(rq == NULL);
	return ((uint32_t)(rq->mq_hctx ? rq->mq_hctx->queue_num : 0)
		<< BLK_MQ_UNIQUE_TAG_BITS) |
	       ((uint32_t)rq->tag & BLK_MQ_UNIQUE_TAG_MASK);
}

/* ---- active queues of a shared map ------------------------------------- */

/* The hctx starts using a shared map: counted in active_queues. */
void blk_mq_tag_busy(struct blk_mq_hw_ctx *hctx)
{
	if (!(hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED))
		return;
	if (__atomic_load_n(&hctx->state, __ATOMIC_RELAXED) &
	    (1ul << BLK_MQ_S_TAG_ACTIVE))
		return;
	if (!(__atomic_fetch_or(&hctx->state, 1ul << BLK_MQ_S_TAG_ACTIVE,
				__ATOMIC_SEQ_CST) &
	      (1ul << BLK_MQ_S_TAG_ACTIVE)))
		(void)__atomic_add_fetch(&hctx->tags->active_queues, 1,
					 __ATOMIC_SEQ_CST);
}

/* It went idle (the timeout work found nothing pending). */
void blk_mq_tag_idle(struct blk_mq_hw_ctx *hctx)
{
	if (!(hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED))
		return;
	if (__atomic_fetch_and(&hctx->state, ~(1ul << BLK_MQ_S_TAG_ACTIVE),
			       __ATOMIC_SEQ_CST) &
	    (1ul << BLK_MQ_S_TAG_ACTIVE)) {
		uint32_t v = __atomic_load_n(&hctx->tags->active_queues,
					     __ATOMIC_RELAXED);

		if (!WARN_ON_ONCE(v == 0))
			(void)__atomic_sub_fetch(&hctx->tags->active_queues, 1,
						 __ATOMIC_SEQ_CST);
		blk_sbq_wake_all(&hctx->tags->bitmap_tags);
	}
}

/* Fair share of a shared map: may this hctx take one more?  (Reserved
 * tags are never limited.) */
static int blk_mq_hctx_may_queue(struct blk_mq_hw_ctx *hctx,
				 const struct blk_sbitmap_queue *bt)
{
	unsigned int users, depth;

	if (!(hctx->flags & BLK_MQ_F_TAG_QUEUE_SHARED))
		return 1;
	users = __atomic_load_n(&hctx->tags->active_queues, __ATOMIC_RELAXED);
	if (!users)
		return 1;
	depth = (bt->depth + users - 1) / users;
	if (depth < BLK_MQ_TAG_SHARE_MIN)
		depth = BLK_MQ_TAG_SHARE_MIN;
	return __atomic_load_n(&hctx->nr_active, __ATOMIC_RELAXED) < depth;
}

/* One try.  The tag number (reserved: 0..; normal: after the reserved
 * ones) or -1. */
static int blk_mq_try_get_tag(struct blk_mq_hw_ctx *hctx, int reserved,
			      int fair)
{
	struct blk_mq_tags *tags = hctx->tags;
	struct blk_sbitmap_queue *bt;
	int nr;

	if (reserved) {
		if (WARN_ON_ONCE(!tags->nr_reserved_tags))
			return -1;
		bt = &tags->breserved_tags;
	} else {
		bt = &tags->bitmap_tags;
		if (fair && !blk_mq_hctx_may_queue(hctx, bt))
			return -1;
	}
	nr = blk_sbq_get(bt);
	if (nr < 0)
		return -1;
	return reserved ? nr : nr + (int)tags->nr_reserved_tags;
}

/* A tag of `hctx': at once, or -- unless nowait, and only where the
 * caller may sleep -- after waiting for one.  `fair': a submitter's
 * request on a shared map is held to its share.  -1 when none (nowait,
 * or the queue is dying).  Before sleeping the hctx is run once (what is
 * waiting there may be what frees a tag). */
int blk_mq_get_tag(struct blk_mq_hw_ctx *hctx, int reserved, int nowait,
		   int fair)
{
	struct blk_mq_tags *tags;
	struct blk_sbitmap_queue *bt;
	int tag;

	BUG_ON(hctx == NULL || hctx->tags == NULL);
	tags = hctx->tags;
	blk_mq_tag_busy(hctx);
	tag = blk_mq_try_get_tag(hctx, reserved, fair);
	if (tag >= 0 || nowait || !blk_can_sleep())
		return tag;
	might_sleep();
	bt = reserved ? &tags->breserved_tags : &tags->bitmap_tags;
	for (;;) {
		unsigned int wi;
		struct wait_queue_head *ws;

		if (blk_queue_dying(hctx->queue))
			return -1;
		blk_mq_run_hw_queue(hctx, 0);
		tag = blk_mq_try_get_tag(hctx, reserved, fair);
		if (tag >= 0)
			return tag;
		wi = __atomic_fetch_add(&bt->wake_index, 0, __ATOMIC_RELAXED);
		ws = &bt->ws[wi % BLK_SBQ_WAIT_QUEUES];
		(void)__atomic_add_fetch(&bt->ws_active, 1, __ATOMIC_SEQ_CST);
		/* Counted before the last look: a free after it sees us. */
		(void)wait_event_timeout(*ws,
					 (tag = blk_mq_try_get_tag(hctx, reserved,
								   fair)) >= 0 ||
						 blk_queue_dying(hctx->queue),
					 BLK_SBQ_WAIT_RECHECK_MS);
		(void)__atomic_sub_fetch(&bt->ws_active, 1, __ATOMIC_SEQ_CST);
		if (tag >= 0)
			return tag;
	}
}

/* At most this many contexts taken off the restart list per round (the
 * round runs them outside the lock). */
#define BLK_MQ_RESTART_BATCH 16

/* Restart the dispatch of every hctx that found the shared map empty.
 * The contexts are taken off the list under tags->lock into a local
 * batch -- never walked through restart_next after the unlock, where a
 * dispatcher may queue one again -- and restart_running stays up until
 * the batch was run, so a context that goes away waits for us
 * (blk_mq_tag_wait_cancel_sync) instead of being freed under us. */
static void blk_mq_tag_restart_waiters(struct blk_mq_tags *tags)
{
	struct blk_mq_hw_ctx *batch[BLK_MQ_RESTART_BATCH];
	uint64_t fl;

	while (__atomic_load_n(&tags->nr_restart, __ATOMIC_SEQ_CST)) {
		unsigned int n = 0;

		spin_lock_irqsave(&tags->lock, &fl);
		while (tags->restart_head && n < BLK_MQ_RESTART_BATCH) {
			struct blk_mq_hw_ctx *h = tags->restart_head;

			tags->restart_head = h->restart_next;
			h->restart_next = NULL;
			__atomic_store_n(&h->restart_queued, 0, __ATOMIC_RELEASE);
			(void)__atomic_sub_fetch(&tags->nr_restart, 1,
						 __ATOMIC_SEQ_CST);
			batch[n++] = h;
		}
		if (n)
			(void)__atomic_add_fetch(&tags->restart_running, 1,
						 __ATOMIC_SEQ_CST);
		spin_unlock_irqrestore(&tags->lock, fl);
		if (!n)
			return;
		for (unsigned int i = 0; i < n; i++)
			blk_mq_run_hw_queue(batch[i], 1);
		(void)__atomic_sub_fetch(&tags->restart_running, 1,
					 __ATOMIC_SEQ_CST);
	}
}

/* An hctx's dispatch found no tag of a shared map: run it when one is
 * freed.  Returns 1 when queued (0: already). */
int blk_mq_tag_wait_restart(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_tags *tags = hctx->tags;
	uint64_t fl;
	int queued = 0;

	spin_lock_irqsave(&tags->lock, &fl);
	if (!__atomic_load_n(&hctx->restart_queued, __ATOMIC_RELAXED)) {
		hctx->restart_next = tags->restart_head;
		tags->restart_head = hctx;
		__atomic_store_n(&hctx->restart_queued, 1, __ATOMIC_RELEASE);
		(void)__atomic_add_fetch(&tags->nr_restart, 1, __ATOMIC_SEQ_CST);
		queued = 1;
	}
	spin_unlock_irqrestore(&tags->lock, fl);
	return queued;
}

/* Take an hctx off the restart list (its queue goes away). */
void blk_mq_tag_wait_cancel(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_tags *tags = hctx->tags;
	uint64_t fl;

	if (!tags)
		return;
	spin_lock_irqsave(&tags->lock, &fl);
	if (__atomic_load_n(&hctx->restart_queued, __ATOMIC_RELAXED)) {
		struct blk_mq_hw_ctx **pp = &tags->restart_head;

		while (*pp && *pp != hctx)
			pp = &(*pp)->restart_next;
		if (*pp) {
			*pp = hctx->restart_next;
			(void)__atomic_sub_fetch(&tags->nr_restart, 1,
						 __ATOMIC_SEQ_CST);
		}
		hctx->restart_next = NULL;
		__atomic_store_n(&hctx->restart_queued, 0, __ATOMIC_RELEASE);
	}
	spin_unlock_irqrestore(&tags->lock, fl);
}

void blk_mq_tag_wait_cancel_sync(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_tags *tags = hctx->tags;

	if (!tags)
		return;
	blk_mq_tag_wait_cancel(hctx);
	/* A waker that took it off the list before us may still be about to
	 * run it: its round is short (it only queues the runs). */
	while (__atomic_load_n(&tags->restart_running, __ATOMIC_SEQ_CST))
		lapic_delay_us(1);
}

/* Give a tag back.  Any context. */
void blk_mq_put_tag(struct blk_mq_tags *tags, unsigned int tag)
{
	uint32_t cpu = blk_tag_cpu();

	BUG_ON(tags == NULL);
	BUG_ON(tag >= tags->nr_tags);
	if (tag >= tags->nr_reserved_tags)
		blk_sbq_clear(&tags->bitmap_tags, tag - tags->nr_reserved_tags,
			      cpu);
	else
		blk_sbq_clear(&tags->breserved_tags, tag, cpu);
	blk_mq_tag_restart_waiters(tags);
}

/* Is `tag' in use (allocated, not freed)? */
int blk_mq_tag_in_use(const struct blk_mq_tags *tags, unsigned int tag)
{
	if (tag >= tags->nr_tags)
		return 0;
	if (tag >= tags->nr_reserved_tags)
		return blk_sbq_test_bit(&tags->bitmap_tags,
					tag - tags->nr_reserved_tags);
	return blk_sbq_test_bit(&tags->breserved_tags, tag);
}

/* Tags in use (sysfs, diagnostics). */
unsigned int blk_mq_tags_busy(const struct blk_mq_tags *tags)
{
	return blk_sbq_weight(&tags->bitmap_tags) +
	       blk_sbq_weight(&tags->breserved_tags);
}

/* ==========================================================================
 * Busy-tag iteration
 * ========================================================================== */

/* The request under `tag' with a reference, or NULL. */
static struct blk_request *blk_mq_find_and_get_req(struct blk_mq_tags *tags,
						   unsigned int tag)
{
	struct blk_request *rq;
	uint64_t fl;

	spin_lock_irqsave(&tags->lock, &fl);
	rq = __atomic_load_n(&tags->rqs[tag], __ATOMIC_ACQUIRE);
	if (rq && (rq->magic != BLK_REQUEST_MAGIC ||
		   !refcount_inc_not_zero(&rq->mq_ref)))
		rq = NULL;
	spin_unlock_irqrestore(&tags->lock, fl);
	return rq;
}

/* Walk the busy tags of `tags'.  `q' non-NULL: only its requests;
 * `started': only requests the driver started.  fn returns 0 to stop.
 * Returns 0 when stopped early. */
int blk_mq_tags_iter(struct blk_mq_tags *tags, struct blk_queue *q,
		     struct blk_mq_hw_ctx *hctx, int started,
		     int (*fn)(struct blk_request *rq, void *data), void *data)
{
	for (int pool = 0; pool < 2; pool++) {
		struct blk_sbitmap_queue *bt = pool ? &tags->bitmap_tags :
						      &tags->breserved_tags;
		unsigned int off = pool ? tags->nr_reserved_tags : 0;

		for (unsigned int i = 0; i < bt->map_nr; i++) {
			uint64_t w = __atomic_load_n(&bt->map[i].word,
						     __ATOMIC_ACQUIRE);

			while (w) {
				unsigned int b = (unsigned int)__builtin_ctzll(w);
				unsigned int tag = off + (i << bt->shift) + b;
				struct blk_request *rq;
				int cont = 1;

				w &= w - 1;
				if (tag >= tags->nr_tags)
					break;
				rq = blk_mq_find_and_get_req(tags, tag);
				if (!rq)
					continue;
				if ((!q || rq->queue == q) &&
				    (!hctx || rq->mq_hctx == hctx) &&
				    (!started || blk_mq_request_started(rq)))
					cont = fn(rq, data);
				blk_mq_put_rq_ref(rq);
				if (!cont)
					return 0;
			}
		}
	}
	return 1;
}

void blk_mq_tagset_busy_iter(struct blk_mq_tag_set *set, busy_tag_iter_fn fn,
			     void *data)
{
	unsigned int n;

	if (!set || !fn || !set->tags)
		return;
	n = (set->flags & BLK_MQ_F_TAG_HCTX_SHARED) ? 1 : set->nr_hw_queues;
	for (unsigned int i = 0; i < n; i++) {
		struct blk_mq_tags *tags = (set->flags & BLK_MQ_F_TAG_HCTX_SHARED) ?
						   set->shared_tags :
						   set->tags[i];

		if (tags && !blk_mq_tags_iter(tags, NULL, NULL, 1, fn, data))
			return;
	}
}

static int blk_mq_count_completed_fn(struct blk_request *rq, void *data)
{
	unsigned int *count = (unsigned int *)data;

	if (blk_mq_request_completed(rq))
		(*count)++;
	return 1;
}

/* (A request the driver ended is IDLE at once -- blk_mq_end_hook -- even
 * while a timeout scan or an iterator still holds a reference on it, so
 * only completions still on their way are counted here: a timeout handler
 * that cancels the tag set while it holds such a request does not wait for
 * itself.) */
void blk_mq_tagset_wait_completed_request(struct blk_mq_tag_set *set)
{
	might_sleep();
	for (;;) {
		unsigned int count = 0;

		blk_mq_tagset_busy_iter(set, blk_mq_count_completed_fn, &count);
		if (!count)
			break;
		/* The completions run in a softirq or a driver context of
		 * their own: give them the processor (the established 5 ms
		 * pause between looks). */
		blk_mq_msleep(5);
	}
}
