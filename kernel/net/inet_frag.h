// LikeOS — fragment reassembly queues (the protocol-independent part)
//
// A queue collects the fragments of one datagram, keyed by (source,
// destination, identification, protocol).  Queues live in a hash table and
// on an age list (least recently fed first); a fixed pool supplies them.
// The bytes held by every queue are accounted (the skbs' truesize): above
// high_thresh the oldest queues are evicted until the total is below
// low_thresh.  Each queue has a timer; when it fires before the datagram is
// complete the owner's expire() runs (IPv4 answers with ICMP time exceeded
// when it had fragment 0) and the queue is dropped.
//
// Lifetime: q->refcnt counts the hash table's reference (while hashed), the
// timer's (while armed) and every lookup's.  inet_frag_kill() unhashes the
// queue and disarms its timer, dropping those two references; the memory
// (and the fragments) go back when the last reference is put.  A queue is
// never freed with a lock held.
//
// Lock order: q->lock -> table lock -> (timer wheel lock).  The table lock
// is never held while a queue lock is taken.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_NET_INET_FRAG_H
#define LIKEOS_NET_INET_FRAG_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/net/skb.h>
#include <kernel/net/net_timer.h>

#define INET_FRAG_FIRST_IN 0x01 // fragment 0 is here
#define INET_FRAG_LAST_IN 0x02 // the last fragment is here (len is final)
#define INET_FRAG_COMPLETE 0x04 // killed or reassembled: no more input
#define INET_FRAG_HASHED 0x08

#define INET_FRAG_HASH_SIZE 1024

struct frag_key {
	uint32_t saddr, daddr; // host order
	uint16_t id;
	uint8_t proto;
	uint8_t _pad;
};

struct inet_frags;

struct inet_frag_queue {
	struct inet_frag_queue *hnext; // hash chain
	struct inet_frag_queue *lru_prev, *lru_next; // age list
	struct inet_frag_queue *free_next; // pool free list
	struct inet_frags *owner;
	spinlock_t lock;
	refcount_t refcnt;
	struct frag_key key;
	uint8_t flags; // INET_FRAG_*
	uint8_t in_use;
	uint16_t max_size; // largest fragment received (datagram bytes)
	uint32_t len; // payload length of the datagram (final with LAST_IN)
	uint32_t meat; // payload bytes received
	uint32_t mem; // truesize of the fragments held
	uint32_t nfrags;
	sk_buff_t *fragments; // sorted by offset, linked through skb->next
	sk_buff_t *last; // the one with the highest offset
	net_timer_t timer;
};

struct inet_frags {
	spinlock_t lock;
	struct inet_frag_queue *hash[INET_FRAG_HASH_SIZE];
	struct inet_frag_queue *lru_head, *lru_tail; // oldest first
	struct inet_frag_queue *free;
	uint32_t nqueues;
	uint32_t rnd; // hash secret
	volatile uint64_t mem; // bytes held by every queue
	uint64_t high_thresh, low_thresh;
	uint64_t timeout_ms;
	uint64_t evicted;
	// The datagram timed out: q is held for the call, unlocked.
	void (*expire)(struct inet_frag_queue *q);
};

void inet_frags_init(struct inet_frags *f, struct inet_frag_queue *pool,
		     uint32_t n, uint64_t timeout_ms, uint64_t high,
		     uint64_t low, void (*expire)(struct inet_frag_queue *q));
// The queue for `key', held, created (empty, timer armed) when there is
// none.  NULL when no queue can be had.
struct inet_frag_queue *inet_frag_find(struct inet_frags *f,
				       const struct frag_key *key);
// Mark complete, unhash, disarm.  q->lock held; the caller holds a ref.
void inet_frag_kill(struct inet_frag_queue *q);
void inet_frag_put(struct inet_frag_queue *q);
// Account `bytes' more (or fewer) held by q (q->lock held).
void inet_frag_mem_add(struct inet_frag_queue *q, int64_t bytes);
// Take the fragments out of a killed queue (q->lock held): the caller owns
// the list (linked through skb->next) and its memory is no longer counted.
sk_buff_t *inet_frag_steal(struct inet_frag_queue *q);
// Move q to the young end of the age list (q->lock held).
void inet_frag_touch(struct inet_frag_queue *q);
// Over high_thresh: evict the oldest queues until below low_thresh.
// No lock held.  Returns the number evicted.
uint32_t inet_frag_evict(struct inet_frags *f);

static inline uint64_t inet_frag_mem(const struct inet_frags *f)
{
	return __atomic_load_n(&f->mem, __ATOMIC_RELAXED);
}

#endif // LIKEOS_NET_INET_FRAG_H
