// LikeOS — fragment reassembly queues (see inet_frag.h)
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/inet_frag.h>
#include <kernel/net/net.h>
#include <kernel/dev/rand/random.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>

static uint32_t frag_hashfn(const struct inet_frags *f,
			    const struct frag_key *k)
{
	uint32_t h = k->saddr * 0x9E3779B1u;

	h ^= k->daddr + f->rnd;
	h *= 0x85EBCA77u;
	h ^= ((uint32_t)k->id << 8) ^ k->proto;
	h ^= h >> 15;
	h *= 0xC2B2AE3Du;
	h ^= h >> 13;
	return h & (INET_FRAG_HASH_SIZE - 1);
}

static int frag_key_eq(const struct frag_key *a, const struct frag_key *b)
{
	return a->saddr == b->saddr && a->daddr == b->daddr &&
	       a->id == b->id && a->proto == b->proto;
}

static void inet_frag_timer_fn(net_timer_t *t);

void inet_frags_init(struct inet_frags *f, struct inet_frag_queue *pool,
		     uint32_t n, uint64_t timeout_ms, uint64_t high,
		     uint64_t low, void (*expire)(struct inet_frag_queue *q))
{
	spinlock_init(&f->lock, "inet_frags");
	mm_memset(f->hash, 0, sizeof(f->hash));
	f->lru_head = f->lru_tail = NULL;
	f->free = NULL;
	for (uint32_t i = n; i-- > 0;) {
		mm_memset(&pool[i], 0, sizeof(pool[i]));
		pool[i].owner = f;
		spinlock_init(&pool[i].lock, "frag_q");
		net_timer_setup(&pool[i].timer, inet_frag_timer_fn);
		pool[i].free_next = f->free;
		f->free = &pool[i];
	}
	f->nqueues = 0;
	f->rnd = random_u32();
	f->mem = 0;
	f->high_thresh = high;
	f->low_thresh = low;
	f->timeout_ms = timeout_ms;
	f->evicted = 0;
	f->expire = expire;
}

// ---- age list (table lock held) ----

static void lru_unlink(struct inet_frags *f, struct inet_frag_queue *q)
{
	if (q->lru_prev)
		q->lru_prev->lru_next = q->lru_next;
	else if (f->lru_head == q)
		f->lru_head = q->lru_next;
	if (q->lru_next)
		q->lru_next->lru_prev = q->lru_prev;
	else if (f->lru_tail == q)
		f->lru_tail = q->lru_prev;
	q->lru_prev = q->lru_next = NULL;
}

static void lru_append(struct inet_frags *f, struct inet_frag_queue *q)
{
	q->lru_next = NULL;
	q->lru_prev = f->lru_tail;
	if (f->lru_tail)
		f->lru_tail->lru_next = q;
	else
		f->lru_head = q;
	f->lru_tail = q;
}

void inet_frag_touch(struct inet_frag_queue *q)
{
	struct inet_frags *f = q->owner;
	uint64_t fl;

	spin_lock_irqsave(&f->lock, &fl);
	if (q->flags & INET_FRAG_HASHED) {
		lru_unlink(f, q);
		lru_append(f, q);
	}
	spin_unlock_irqrestore(&f->lock, fl);
}

/* Unlink from the hash and the age list (table lock held). */
static void frag_unhash_locked(struct inet_frags *f, struct inet_frag_queue *q)
{
	struct inet_frag_queue **pp = &f->hash[frag_hashfn(f, &q->key)];

	for (; *pp; pp = &(*pp)->hnext) {
		if (*pp == q) {
			*pp = q->hnext;
			break;
		}
	}
	q->hnext = NULL;
	lru_unlink(f, q);
	q->flags &= (uint8_t)~INET_FRAG_HASHED;
	f->nqueues--;
}

static struct inet_frag_queue *frag_lookup_locked(struct inet_frags *f,
						  const struct frag_key *k)
{
	for (struct inet_frag_queue *q = f->hash[frag_hashfn(f, k)]; q;
	     q = q->hnext)
		if (frag_key_eq(&q->key, k))
			return q;
	return NULL;
}

struct inet_frag_queue *inet_frag_find(struct inet_frags *f,
				       const struct frag_key *key)
{
	struct inet_frag_queue *q;
	uint64_t fl;

	for (int attempt = 0; attempt < 2; attempt++) {
		spin_lock_irqsave(&f->lock, &fl);
		q = frag_lookup_locked(f, key);
		if (q) {
			refcount_inc(&q->refcnt);
			spin_unlock_irqrestore(&f->lock, fl);
			return q;
		}
		q = f->free;
		if (q) {
			f->free = q->free_next;
			q->free_next = NULL;
			q->in_use = 1;
			q->key = *key;
			q->flags = INET_FRAG_HASHED;
			q->max_size = 0;
			q->len = q->meat = q->mem = q->nfrags = 0;
			q->fragments = q->last = NULL;
			// the table's, the timer's and the caller's
			refcount_set(&q->refcnt, 3);
			{
				uint32_t h = frag_hashfn(f, key);

				q->hnext = f->hash[h];
				f->hash[h] = q;
			}
			lru_append(f, q);
			f->nqueues++;
			net_timer_mod(&q->timer,
				      timer_ticks() +
					      net_ms_to_ticks(f->timeout_ms));
			spin_unlock_irqrestore(&f->lock, fl);
			return q;
		}
		spin_unlock_irqrestore(&f->lock, fl);
		/* Every queue is in use: make room by dropping the oldest
		 * (the same thing memory pressure does), once. */
		if (attempt == 0) {
			struct inet_frag_queue *old;

			spin_lock_irqsave(&f->lock, &fl);
			old = f->lru_head;
			if (old)
				refcount_inc(&old->refcnt);
			spin_unlock_irqrestore(&f->lock, fl);
			if (!old)
				return NULL;
			spin_lock_irqsave(&old->lock, &fl);
			if (!(old->flags & INET_FRAG_COMPLETE)) {
				inet_frag_kill(old);
				__atomic_fetch_add(&f->evicted, 1,
						   __ATOMIC_RELAXED);
			}
			spin_unlock_irqrestore(&old->lock, fl);
			inet_frag_put(old);
		}
	}
	return NULL;
}

void inet_frag_kill(struct inet_frag_queue *q)
{
	struct inet_frags *f = q->owner;
	int drops = 0;
	uint64_t fl;

	lockdep_assert_held(&q->lock);
	if (q->flags & INET_FRAG_COMPLETE)
		return;
	q->flags |= INET_FRAG_COMPLETE;
	if (net_timer_del(&q->timer))
		drops++; // the timer's reference
	spin_lock_irqsave(&f->lock, &fl);
	if (q->flags & INET_FRAG_HASHED) {
		frag_unhash_locked(f, q);
		drops++; // the table's reference
	}
	spin_unlock_irqrestore(&f->lock, fl);
	/* The caller's reference keeps the queue alive: these never reach
	 * zero here. */
	while (drops--)
		BUG_ON(refcount_dec_and_test(&q->refcnt));
}

void inet_frag_mem_add(struct inet_frag_queue *q, int64_t bytes)
{
	q->mem = (uint32_t)((int64_t)q->mem + bytes);
	__atomic_fetch_add(&q->owner->mem, (uint64_t)bytes, __ATOMIC_RELAXED);
}

sk_buff_t *inet_frag_steal(struct inet_frag_queue *q)
{
	sk_buff_t *list = q->fragments;

	lockdep_assert_held(&q->lock);
	BUG_ON(!(q->flags & INET_FRAG_COMPLETE));
	q->fragments = q->last = NULL;
	__atomic_fetch_sub(&q->owner->mem, (uint64_t)q->mem, __ATOMIC_RELAXED);
	q->mem = 0;
	return list;
}

void inet_frag_put(struct inet_frag_queue *q)
{
	struct inet_frags *f;
	sk_buff_t *skb;
	uint64_t fl;

	if (!q || !refcount_dec_and_test(&q->refcnt))
		return;
	f = q->owner;
	BUG_ON(!(q->flags & INET_FRAG_COMPLETE));
	BUG_ON(q->flags & INET_FRAG_HASHED);
	BUG_ON(net_timer_pending(&q->timer));
	skb = q->fragments;
	q->fragments = q->last = NULL;
	__atomic_fetch_sub(&f->mem, (uint64_t)q->mem, __ATOMIC_RELAXED);
	q->mem = 0;
	while (skb) {
		sk_buff_t *next = skb->next;

		skb->next = skb->prev = NULL;
		skb_put(skb);
		skb = next;
	}
	spin_lock_irqsave(&f->lock, &fl);
	q->in_use = 0;
	q->free_next = f->free;
	f->free = q;
	spin_unlock_irqrestore(&f->lock, fl);
}

static void inet_frag_timer_fn(net_timer_t *t)
{
	struct inet_frag_queue *q = (struct inet_frag_queue *)((uint8_t *)t -
		__builtin_offsetof(struct inet_frag_queue, timer));

	// the timer's reference is ours now
	q->owner->expire(q);
	inet_frag_put(q);
}

uint32_t inet_frag_evict(struct inet_frags *f)
{
	uint32_t n = 0;

	if (inet_frag_mem(f) <= f->high_thresh)
		return 0;
	while (inet_frag_mem(f) > f->low_thresh) {
		struct inet_frag_queue *q;
		uint64_t fl;

		spin_lock_irqsave(&f->lock, &fl);
		q = f->lru_head;
		if (q)
			refcount_inc(&q->refcnt);
		spin_unlock_irqrestore(&f->lock, fl);
		if (!q)
			break;
		spin_lock_irqsave(&q->lock, &fl);
		inet_frag_kill(q);
		spin_unlock_irqrestore(&q->lock, fl);
		inet_frag_put(q);
		n++;
	}
	__atomic_fetch_add(&f->evicted, n, __ATOMIC_RELAXED);
	return n;
}
