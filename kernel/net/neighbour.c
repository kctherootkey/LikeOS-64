// LikeOS — neighbour subsystem: the NUD state machine, the table and its
// garbage collector
//
// See kernel/net/neighbour.h for the states, the lifetime rules and the
// lock order.  The protocol specifics (how to solicit, what a failure
// reports, which addresses need no resolution) come from the table's ops;
// kernel/net/arp.c is the IPv4 user.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/neighbour.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/skb.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/rand/random.h>
#include <kernel/uapi/bug.h>

/* Entries come from a static pool sized for the largest table (its
 * gc_thresh3), so they can be created in any context -- the transmit path
 * runs with interrupts off under protocol locks, where the slab allocator
 * may not be called. */
#define NEIGH_POOL_SIZE 1024

static neighbour_t neigh_pool[NEIGH_POOL_SIZE];
static neighbour_t *neigh_free_list;
static uint32_t neigh_free_count;
static spinlock_t neigh_pool_lock = SPINLOCK_INIT("neigh_pool");
static int neigh_pool_ready;

// Tick arithmetic that survives the counter starting near zero.
static inline int tick_before(uint64_t a, uint64_t b)
{
	return (int64_t)(a - b) < 0;
}

static inline int tick_after_eq(uint64_t a, uint64_t b)
{
	return (int64_t)(a - b) >= 0;
}

static inline uint64_t ms2t(uint32_t ms)
{
	return net_ms_to_ticks(ms);
}

static void neigh_pool_init(void)
{
	if (neigh_pool_ready)
		return;
	neigh_free_list = NULL;
	for (int i = NEIGH_POOL_SIZE - 1; i >= 0; i--) {
		neigh_pool[i].free_next = neigh_free_list;
		neigh_free_list = &neigh_pool[i];
	}
	neigh_free_count = NEIGH_POOL_SIZE;
	neigh_pool_ready = 1;
}

static neighbour_t *neigh_pool_get(void)
{
	neighbour_t *n;
	uint64_t f;

	spin_lock_irqsave(&neigh_pool_lock, &f);
	n = neigh_free_list;
	if (n) {
		neigh_free_list = n->free_next;
		neigh_free_count--;
	}
	spin_unlock_irqrestore(&neigh_pool_lock, f);
	return n;
}

static void neigh_pool_put(neighbour_t *n)
{
	uint64_t f;

	spin_lock_irqsave(&neigh_pool_lock, &f);
	n->free_next = neigh_free_list;
	neigh_free_list = n;
	neigh_free_count++;
	spin_unlock_irqrestore(&neigh_pool_lock, f);
}

static inline uint32_t neigh_hash(uint32_t key, const net_device_t *dev)
{
	uint32_t h = key ^ (uint32_t)((uintptr_t)dev >> 4);

	h ^= h >> 16;
	h *= 0x45d9f3bU;
	h ^= h >> 16;
	return h & (NEIGH_HASH_SIZE - 1);
}

static void neigh_destroy(neighbour_t *n)
{
	struct neigh_table *tbl = n->tbl;

	WARN_ON(!n->dead); /* freed while still in the table */
	WARN_ON(net_timer_pending(&n->timer)); /* a timer holds a reference */
	skb_queue_purge(&n->arp_queue);
	tbl->stats.destroys++;
	n->tbl = NULL;
	n->dev = NULL;
	neigh_pool_put(n);
}

void neigh_release(neighbour_t *n)
{
	if (n && refcount_dec_and_test(&n->refcnt))
		neigh_destroy(n);
}

/* Drop a reference the caller knows is not the last (it holds another). */
static void neigh_put_not_last(neighbour_t *n)
{
	if (refcount_dec_and_test(&n->refcnt)) {
		WARN_ON_ONCE(1); /* dropped the last reference under n->lock */
		refcount_set(&n->refcnt, 1);
	}
}

static void neigh_randomize_reachable(struct neigh_table *tbl);

// ---- timer (the pending timer holds a reference) ----

static void neigh_add_timer(neighbour_t *n, uint64_t when)
{
	if (!net_timer_mod(&n->timer, when))
		neigh_hold(n);
}

static int neigh_del_timer(neighbour_t *n)
{
	if (net_timer_del(&n->timer)) {
		neigh_put_not_last(n);
		return 1;
	}
	return 0;
}

static uint32_t neigh_max_probes(const neighbour_t *n)
{
	const struct neigh_parms *p = &n->tbl->parms;

	return p->ucast_probes + p->app_probes +
	       ((n->nud_state & NUD_INCOMPLETE) ? p->mcast_probes :
						  p->mcast_reprobes);
}

/* A probe for `n' is due: what the solicitation needs, captured under
 * n->lock (held); the caller sends it after unlocking. */
struct neigh_probe_req {
	int send;
	int unicast;
	uint32_t saddr;
	uint8_t ha[NEIGH_HALEN];
};

static void neigh_probe_prepare(neighbour_t *n, struct neigh_probe_req *r)
{
	const struct neigh_parms *p = &n->tbl->parms;
	sk_buff_t *last = skb_peek_tail(&n->arp_queue);
	int32_t left = (int32_t)n->probes - (int32_t)p->ucast_probes;

	r->send = 1;
	r->saddr = 0;
	/* The source address of the packet that waits, so the target can
	 * answer that conversation (the queued skb starts at the IP header). */
	if (last && last->len >= 20) {
		const uint8_t *ip = last->data;

		r->saddr = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) |
			   ((uint32_t)ip[14] << 8) | ip[15];
	}
	r->unicast = left < 0 && (n->nud_state & NUD_VALID);
	if (left >= 0) {
		left -= (int32_t)p->app_probes;
		if (left < 0)
			r->send = 0; // a user-space resolver would ask: none
	}
	mm_memcpy(r->ha, n->ha, NEIGH_HALEN);
	n->probes++;
}

static void neigh_probe_send(neighbour_t *n, const struct neigh_probe_req *r)
{
	if (!r->send || !n->tbl->ops->solicit)
		return;
	n->tbl->stats.solicits++;
	n->tbl->ops->solicit(n, r->saddr, r->unicast, r->ha);
}

/* FAILED: take the waiting packets out for their error reports. */
static sk_buff_t *neigh_invalidate_locked(neighbour_t *n)
{
	n->tbl->stats.res_failed++;
	n->arp_queue_bytes = 0;
	return skb_queue_take_all(&n->arp_queue, NULL);
}

static void neigh_report_errors(neighbour_t *n, sk_buff_t *list)
{
	while (list) {
		sk_buff_t *skb = list;

		list = skb->next;
		skb->next = skb->prev = NULL;
		if (n->tbl->ops->error_report)
			n->tbl->ops->error_report(n, skb);
		else
			skb_put(skb);
	}
}

static void neigh_timer_handler(net_timer_t *t)
{
	neighbour_t *n = (neighbour_t *)((char *)t -
					 __builtin_offsetof(neighbour_t, timer));
	const struct neigh_parms *p = &n->tbl->parms;
	struct neigh_probe_req req = { 0 };
	sk_buff_t *failed = NULL;
	uint64_t now, next, f;
	uint8_t state;

	spin_lock_irqsave(&n->lock, &f);
	state = n->nud_state;
	now = timer_ticks();
	next = now + ms2t(1000);
	if (!(state & NUD_IN_TIMER) || n->dead)
		goto out;

	if (state & NUD_REACHABLE) {
		uint64_t reach = ms2t(n->tbl->reachable_time_ms);

		if (tick_before(now, n->confirmed + reach) ||
		    now == n->confirmed + reach) {
			next = n->confirmed + reach;
		} else if (!tick_before(n->used + ms2t(p->delay_probe_time_ms),
					now)) {
			n->nud_state = NUD_DELAY;
			n->updated = now;
			next = now + ms2t(p->delay_probe_time_ms);
		} else {
			n->nud_state = NUD_STALE;
			n->updated = now;
		}
	} else if (state & NUD_DELAY) {
		if (!tick_before(n->confirmed + ms2t(p->delay_probe_time_ms),
				 now)) {
			n->nud_state = NUD_REACHABLE;
			n->updated = now;
			next = n->confirmed + ms2t(n->tbl->reachable_time_ms);
		} else {
			n->nud_state = NUD_PROBE;
			n->updated = now;
			n->probes = 0;
			next = now + ms2t(p->retrans_time_ms);
		}
	} else {
		// INCOMPLETE, PROBE
		next = now + ms2t(p->retrans_time_ms);
	}

	if ((n->nud_state & (NUD_INCOMPLETE | NUD_PROBE)) &&
	    n->probes >= neigh_max_probes(n)) {
		n->nud_state = NUD_FAILED;
		n->updated = now;
		failed = neigh_invalidate_locked(n);
	}

	if (n->nud_state & NUD_IN_TIMER) {
		if (tick_before(next, now + 1))
			next = now + 1;
		neigh_add_timer(n, next);
	}
	if (n->nud_state & (NUD_INCOMPLETE | NUD_PROBE))
		neigh_probe_prepare(n, &req);
out:
	spin_unlock_irqrestore(&n->lock, f);
	if (req.send)
		neigh_probe_send(n, &req);
	neigh_report_errors(n, failed);
	neigh_release(n); // the timer's reference
}

// ---- table ----

static neighbour_t *neigh_find_locked(struct neigh_table *tbl, uint32_t key,
				      net_device_t *dev)
{
	for (neighbour_t *n = tbl->hash[neigh_hash(key, dev)]; n; n = n->hnext)
		if (n->key == key && n->dev == dev)
			return n;
	return NULL;
}

neighbour_t *neigh_lookup(struct neigh_table *tbl, uint32_t key,
			  net_device_t *dev)
{
	neighbour_t *n;
	uint64_t f;

	spin_lock_irqsave(&tbl->lock, &f);
	tbl->stats.lookups++;
	n = neigh_find_locked(tbl, key, dev);
	if (n) {
		neigh_hold(n);
		tbl->stats.hits++;
	}
	spin_unlock_irqrestore(&tbl->lock, f);
	return n;
}

/* Unlink `n' (tbl->lock held) and mark it dead.  The caller drops the
 * table's reference (and the timer's, returned in *timer_ref) after
 * unlocking. */
static void neigh_unlink_locked(struct neigh_table *tbl, neighbour_t *n,
				int *timer_ref)
{
	neighbour_t **pp = &tbl->hash[neigh_hash(n->key, n->dev)];
	uint64_t f;

	for (; *pp; pp = &(*pp)->hnext) {
		if (*pp == n) {
			*pp = n->hnext;
			break;
		}
	}
	n->hnext = NULL;
	tbl->entries--;
	spin_lock_irqsave(&n->lock, &f);
	n->dead = 1;
	*timer_ref = net_timer_del(&n->timer);
	if (refcount_read(&n->refcnt) > 1 + *timer_ref) {
		/* Still in use: make it harmless (no more packets queue on it
		 * and its queue goes). */
		skb_queue_purge(&n->arp_queue);
		n->arp_queue_bytes = 0;
		n->nud_state = (n->nud_state & NUD_VALID) ? NUD_NOARP :
							    NUD_NONE;
	}
	spin_unlock_irqrestore(&n->lock, f);
}

static void neigh_unlinked_release(neighbour_t *n, int timer_ref)
{
	if (timer_ref)
		neigh_release(n);
	neigh_release(n); // the table's
}

/* Unlink up to `max' entries `pick' accepts (tbl->lock held), into
 * victims[]; returns how many. */
typedef int (*neigh_pick_fn)(const neighbour_t *n, uint64_t now,
			     uint64_t arg);

static int neigh_collect_locked(struct neigh_table *tbl, neigh_pick_fn pick,
				uint64_t now, uint64_t arg,
				neighbour_t **victims, int *timer_refs,
				int max)
{
	int nv = 0;

	for (int b = 0; b < NEIGH_HASH_SIZE && nv < max; b++) {
		neighbour_t *n = tbl->hash[b];

		while (n && nv < max) {
			neighbour_t *next = n->hnext;

			if (pick(n, now, arg)) {
				neigh_unlink_locked(tbl, n, &timer_refs[nv]);
				victims[nv++] = n;
			}
			n = next;
		}
	}
	return nv;
}

/* Forced collection: unreferenced, not permanent, and failed, NOARP,
 * unresolved or not updated for 5 s. */
static int neigh_pick_forced(const neighbour_t *n, uint64_t now,
			     uint64_t tref)
{
	uint8_t st = n->nud_state;

	(void)now;
	return refcount_read(&n->refcnt) == 1 && !(st & NUD_PERMANENT) &&
	       (st == NUD_FAILED || st == NUD_NOARP || st == NUD_NONE ||
		tick_before(n->updated, tref));
}

int neigh_forced_gc(struct neigh_table *tbl)
{
	uint64_t now = timer_ticks();
	uint64_t tref = now - ms2t(5000);
	int shrunk = 0, max_clean;
	uint64_t f;

	spin_lock_irqsave(&tbl->lock, &f);
	tbl->stats.forced_gc_runs++;
	tbl->last_flush = now;
	// down to gc_thresh2, at least one
	max_clean = (int)tbl->entries - (int)tbl->gc_thresh2;
	if (max_clean < 1)
		max_clean = 1;
	spin_unlock_irqrestore(&tbl->lock, f);
	while (shrunk < max_clean) {
		neighbour_t *victims[32];
		int timer_refs[32];
		int want = max_clean - shrunk, nv;

		if (want > 32)
			want = 32;
		spin_lock_irqsave(&tbl->lock, &f);
		nv = neigh_collect_locked(tbl, neigh_pick_forced, now, tref,
					  victims, timer_refs, want);
		spin_unlock_irqrestore(&tbl->lock, f);
		for (int i = 0; i < nv; i++)
			neigh_unlinked_release(victims[i], timer_refs[i]);
		shrunk += nv;
		if (nv < want)
			break;
	}
	return shrunk;
}

static neighbour_t *neigh_alloc(struct neigh_table *tbl, uint32_t key,
				net_device_t *dev)
{
	uint32_t entries = __atomic_load_n(&tbl->entries, __ATOMIC_RELAXED);
	uint64_t now = timer_ticks();
	neighbour_t *n;

	if (entries >= tbl->gc_thresh3 ||
	    (entries >= tbl->gc_thresh2 &&
	     tick_after_eq(now, tbl->last_flush + ms2t(5000)))) {
		if (!neigh_forced_gc(tbl) &&
		    __atomic_load_n(&tbl->entries, __ATOMIC_RELAXED) >=
			    tbl->gc_thresh3) {
			tbl->stats.table_fulls++;
			return NULL;
		}
	}
	n = neigh_pool_get();
	if (!n) {
		tbl->stats.table_fulls++;
		return NULL;
	}
	mm_memset(n, 0, sizeof(*n));
	n->tbl = tbl;
	n->dev = dev;
	n->key = key;
	refcount_set(&n->refcnt, 1);
	spinlock_init(&n->lock, "neigh");
	n->nud_state = NUD_NONE;
	n->updated = now;
	n->used = now;
	n->confirmed = now - ms2t(tbl->parms.base_reachable_time_ms) * 2;
	net_timer_setup(&n->timer, neigh_timer_handler);
	skb_queue_init(&n->arp_queue, "arp_queue");
	if (tbl->ops->constructor)
		tbl->ops->constructor(n);
	tbl->stats.allocs++;
	return n;
}

neighbour_t *__neigh_lookup(struct neigh_table *tbl, uint32_t key,
			    net_device_t *dev, int creat)
{
	neighbour_t *n, *old;
	uint64_t f;

	n = neigh_lookup(tbl, key, dev);
	if (n || !creat)
		return n;
	n = neigh_alloc(tbl, key, dev);
	if (!n)
		return NULL;
	spin_lock_irqsave(&tbl->lock, &f);
	old = neigh_find_locked(tbl, key, dev);
	if (old) {
		// lost a race with another creator
		neigh_hold(old);
		spin_unlock_irqrestore(&tbl->lock, f);
		n->dead = 1;
		neigh_release(n);
		return old;
	}
	{
		uint32_t h = neigh_hash(key, dev);

		n->hnext = tbl->hash[h];
		tbl->hash[h] = n;
	}
	tbl->entries++;
	neigh_hold(n); // the caller's; the first one is the table's
	spin_unlock_irqrestore(&tbl->lock, f);
	return n;
}

// ---- output ----

/* Ethernet header for `ha' and hand the frame to the device.  Consumes. */
static int neigh_hh_xmit(neighbour_t *n, sk_buff_t *skb,
			 const uint8_t ha[NEIGH_HALEN])
{
	net_device_t *dev = n->dev;
	eth_header_t *eh;

	if (skb_headroom(skb) < ETH_HLEN || skb_cloned(skb)) {
		skb = skb_realloc_headroom(skb, ETH_HLEN);
		if (!skb) {
			dev->tx_dropped++;
			return -1;
		}
	}
	skb->network_header = (uint16_t)(skb->data - skb->head);
	eh = (eth_header_t *)skb_push(skb, ETH_HLEN);
	mm_memcpy(eh->dst, ha, ETH_ALEN);
	mm_memcpy(eh->src, dev->mac_addr, ETH_ALEN);
	eh->ethertype = net_htons(ETH_P_IP);
	skb_reset_mac_header(skb);
	skb->protocol = ETH_P_IP;
	skb->dev = dev;
	return dev_queue_xmit(skb) == NET_XMIT_SUCCESS ? 0 : -1;
}

int neigh_event_send(neighbour_t *n, sk_buff_t *skb)
{
	const struct neigh_parms *p = &n->tbl->parms;
	struct neigh_probe_req req = { 0 };
	sk_buff_t *drop = NULL;
	uint64_t now = timer_ticks();
	uint64_t f;
	int rc = 0;

	if (__atomic_load_n(&n->used, __ATOMIC_RELAXED) != now)
		__atomic_store_n(&n->used, now, __ATOMIC_RELAXED);
	if (n->nud_state & (NUD_CONNECTED | NUD_DELAY | NUD_PROBE))
		return 0;

	spin_lock_irqsave(&n->lock, &f);
	if (n->dead) {
		spin_unlock_irqrestore(&n->lock, f);
		skb_put(skb);
		return 1;
	}
	if (n->nud_state & (NUD_CONNECTED | NUD_DELAY | NUD_PROBE))
		goto out_unlock;
	if (!(n->nud_state & (NUD_STALE | NUD_INCOMPLETE))) {
		// NONE or FAILED: (re)start resolution
		if (p->mcast_probes + p->app_probes) {
			n->probes = p->ucast_probes;
			n->nud_state = NUD_INCOMPLETE;
			n->updated = now;
			neigh_add_timer(n, now + (ms2t(p->retrans_time_ms) ?
							  ms2t(p->retrans_time_ms) :
							  1));
			neigh_probe_prepare(n, &req);
			/* the probe goes after the packet is queued, so it
			 * can name the packet's source; see below */
		} else {
			n->nud_state = NUD_FAILED;
			n->updated = now;
			spin_unlock_irqrestore(&n->lock, f);
			skb_put(skb);
			return 1;
		}
	} else if (n->nud_state & NUD_STALE) {
		n->nud_state = NUD_DELAY;
		n->updated = now;
		neigh_add_timer(n, now + ms2t(p->delay_probe_time_ms));
	}

	if (n->nud_state == NUD_INCOMPLETE) {
		if (skb) {
			while (n->arp_queue_bytes + skb->truesize >
				       p->queue_len_bytes &&
			       skb_queue_len(&n->arp_queue)) {
				sk_buff_t *old = __skb_dequeue(&n->arp_queue);

				n->arp_queue_bytes -= old->truesize;
				old->next = drop;
				drop = old;
				n->tbl->stats.unres_discards++;
			}
			if (skb->truesize <= p->queue_len_bytes) {
				__skb_queue_tail(&n->arp_queue, skb);
				n->arp_queue_bytes += skb->truesize;
			} else {
				skb->next = drop;
				drop = skb;
				n->tbl->stats.unres_discards++;
			}
			if (req.send && skb->len >= 20) {
				const uint8_t *ip = skb->data;

				req.saddr = ((uint32_t)ip[12] << 24) |
					    ((uint32_t)ip[13] << 16) |
					    ((uint32_t)ip[14] << 8) | ip[15];
			}
		}
		rc = 1;
	}
out_unlock:
	spin_unlock_irqrestore(&n->lock, f);
	if (req.send)
		neigh_probe_send(n, &req);
	while (drop) {
		sk_buff_t *next = drop->next;

		drop->next = drop->prev = NULL;
		skb_put(drop);
		drop = next;
	}
	return rc;
}

int neigh_output(neighbour_t *n, sk_buff_t *skb)
{
	uint8_t ha[NEIGH_HALEN];
	uint64_t f;

	if (n->dead) {
		n->dev->tx_dropped++;
		skb_put(skb);
		return -1;
	}
	if (!(n->nud_state & NUD_CONNECTED) && neigh_event_send(n, skb))
		return 0; // queued for resolution (or dropped)
	spin_lock_irqsave(&n->lock, &f);
	mm_memcpy(ha, n->ha, NEIGH_HALEN);
	spin_unlock_irqrestore(&n->lock, f);
	return neigh_hh_xmit(n, skb, ha);
}

// ---- update ----

int neigh_update(neighbour_t *n, const uint8_t *lladdr, uint8_t new_state,
		 uint32_t flags)
{
	uint8_t newst = new_state, old;
	uint8_t addr[NEIGH_HALEN];
	sk_buff_t *failed = NULL, *resolved = NULL;
	int addr_changed = 0, err = -1;
	uint64_t now = timer_ticks();
	uint64_t f;

	spin_lock_irqsave(&n->lock, &f);
	if (n->dead)
		goto out;
	old = n->nud_state;
	if (!(flags & NEIGH_UPDATE_F_ADMIN) &&
	    (old & (NUD_NOARP | NUD_PERMANENT)))
		goto out;

	if (!(newst & NUD_VALID)) {
		neigh_del_timer(n);
		n->nud_state = newst;
		if (newst != old)
			n->updated = now;
		if ((old & (NUD_INCOMPLETE | NUD_PROBE)) &&
		    (newst & NUD_FAILED))
			failed = neigh_invalidate_locked(n);
		err = 0;
		goto out;
	}

	// The address to use: the given one, or the one we have.
	if (lladdr) {
		mm_memcpy(addr, lladdr, NEIGH_HALEN);
		addr_changed = !(old & NUD_VALID) ||
			       kmemcmp(addr, n->ha, NEIGH_HALEN) != 0;
	} else {
		if (!(old & NUD_VALID))
			goto out; // nothing to resolve with
		mm_memcpy(addr, n->ha, NEIGH_HALEN);
	}

	/* A reply confirms reachability even when nothing else changes. */
	if (newst & NUD_CONNECTED)
		n->confirmed = now;

	err = 0;
	if (old & NUD_VALID) {
		if (addr_changed && !(flags & NEIGH_UPDATE_F_OVERRIDE)) {
			if ((flags & NEIGH_UPDATE_F_WEAK_OVERRIDE) &&
			    (old & NUD_CONNECTED)) {
				// keep the address, but stop trusting it
				mm_memcpy(addr, n->ha, NEIGH_HALEN);
				addr_changed = 0;
				newst = NUD_STALE;
			} else {
				goto out;
			}
		} else if (!addr_changed && newst == NUD_STALE &&
			   !(flags & NEIGH_UPDATE_F_ADMIN)) {
			/* The same address again: a request does not demote
			 * an entry we know better. */
			newst = old;
		}
	}

	/* Only a real change moves the lock-time window. */
	if (newst != old || addr_changed)
		n->updated = now;
	if (newst != old) {
		neigh_del_timer(n);
		if (newst & NUD_PROBE)
			n->probes = 0;
		if (newst & NUD_IN_TIMER)
			neigh_add_timer(
				n, now + ((newst & NUD_REACHABLE) ?
						  ms2t(n->tbl->reachable_time_ms) :
						  0));
		n->nud_state = newst;
	}
	if (addr_changed) {
		mm_memcpy(n->ha, addr, NEIGH_HALEN);
		if (!(newst & NUD_CONNECTED))
			n->confirmed =
				now -
				ms2t(n->tbl->parms.base_reachable_time_ms) * 2;
	}
	if (newst != old && !(old & NUD_VALID)) {
		resolved = skb_queue_take_all(&n->arp_queue, NULL);
		n->arp_queue_bytes = 0;
	}
out:
	spin_unlock_irqrestore(&n->lock, f);
	// Packets that waited for this address go now, in order.
	while (resolved) {
		sk_buff_t *skb = resolved;

		resolved = skb->next;
		skb->next = skb->prev = NULL;
		neigh_hh_xmit(n, skb, addr);
	}
	neigh_report_errors(n, failed);
	return err;
}

neighbour_t *neigh_event_ns(struct neigh_table *tbl, const uint8_t *lladdr,
			    uint32_t key, net_device_t *dev)
{
	neighbour_t *n = __neigh_lookup(tbl, key, dev, 1);

	if (n)
		neigh_update(n, lladdr, NUD_STALE, NEIGH_UPDATE_F_OVERRIDE);
	return n;
}

int neigh_delete(struct neigh_table *tbl, uint32_t key, net_device_t *dev)
{
	int count = 0;

	for (;;) {
		neighbour_t *n = NULL;
		int timer_ref = 0;
		uint64_t f;

		spin_lock_irqsave(&tbl->lock, &f);
		for (int b = 0; b < NEIGH_HASH_SIZE && !n; b++)
			for (neighbour_t *e = tbl->hash[b]; e; e = e->hnext)
				if (e->key == key && (!dev || e->dev == dev)) {
					n = e;
					break;
				}
		if (n)
			neigh_hold(n);
		spin_unlock_irqrestore(&tbl->lock, f);
		if (!n)
			return count;
		neigh_update(n, NULL, NUD_FAILED,
			     NEIGH_UPDATE_F_OVERRIDE | NEIGH_UPDATE_F_ADMIN);
		spin_lock_irqsave(&tbl->lock, &f);
		if (!n->dead) {
			neigh_unlink_locked(tbl, n, &timer_ref);
			spin_unlock_irqrestore(&tbl->lock, f);
			neigh_unlinked_release(n, timer_ref);
			count++;
		} else {
			spin_unlock_irqrestore(&tbl->lock, f);
		}
		neigh_release(n);
	}
}

void neigh_flush_dev(struct neigh_table *tbl, net_device_t *dev,
		     int skip_perm)
{
	for (;;) {
		neighbour_t *victims[32];
		int timer_refs[32];
		int nv = 0;
		uint64_t f;

		spin_lock_irqsave(&tbl->lock, &f);
		for (int b = 0; b < NEIGH_HASH_SIZE && nv < 32; b++) {
			neighbour_t *n = tbl->hash[b];

			while (n && nv < 32) {
				neighbour_t *next = n->hnext;

				if ((!dev || n->dev == dev) &&
				    !(skip_perm &&
				      (n->nud_state & NUD_PERMANENT))) {
					neigh_unlink_locked(tbl, n,
							    &timer_refs[nv]);
					victims[nv++] = n;
				}
				n = next;
			}
		}
		spin_unlock_irqrestore(&tbl->lock, f);
		for (int i = 0; i < nv; i++)
			neigh_unlinked_release(victims[i], timer_refs[i]);
		if (nv < 32)
			return;
	}
}

int neigh_dump(struct neigh_table *tbl, struct neigh_dump_entry *out,
	       int max, uint8_t states, int skip)
{
	int count = 0;
	uint64_t f;

	spin_lock_irqsave(&tbl->lock, &f);
	for (int b = 0; b < NEIGH_HASH_SIZE && count < max; b++) {
		for (neighbour_t *n = tbl->hash[b]; n && count < max;
		     n = n->hnext) {
			uint64_t nf;

			spin_lock_irqsave(&n->lock, &nf);
			if ((n->nud_state & states) && skip > 0) {
				skip--;
			} else if (n->nud_state & states) {
				out[count].key = n->key;
				mm_memcpy(out[count].ha, n->ha, NEIGH_HALEN);
				out[count].nud_state = n->nud_state;
				out[count]._pad = 0;
				out[count].dev = n->dev;
				count++;
			}
			spin_unlock_irqrestore(&n->lock, nf);
		}
	}
	spin_unlock_irqrestore(&tbl->lock, f);
	return count;
}

// ---- garbage collection ----

static void neigh_randomize_reachable(struct neigh_table *tbl)
{
	uint32_t base = tbl->parms.base_reachable_time_ms;

	// uniformly in [base/2, 3*base/2)
	tbl->reachable_time_ms = base ? base / 2 + random_u32() % base : 0;
	tbl->last_rand = timer_ticks();
}

/* Periodic collection: unreferenced, not permanent, not in a timed
 * state, and failed or unused for gc_staletime. */
static int neigh_pick_stale(const neighbour_t *n, uint64_t now,
			    uint64_t stale)
{
	uint8_t st = n->nud_state;

	return !(st & (NUD_PERMANENT | NUD_IN_TIMER)) &&
	       refcount_read(&n->refcnt) == 1 &&
	       (st == NUD_FAILED || tick_after_eq(now, n->used + stale));
}

void neigh_periodic_gc(struct neigh_table *tbl)
{
	uint64_t now = timer_ticks();
	uint64_t stale = ms2t(tbl->parms.gc_staletime_ms);
	int scan;
	uint64_t f;

	spin_lock_irqsave(&tbl->lock, &f);
	tbl->stats.periodic_gc_runs++;
	if (tick_after_eq(now, tbl->last_rand + ms2t(300000)))
		neigh_randomize_reachable(tbl);
	// a small table is left alone
	scan = tbl->entries >= tbl->gc_thresh1;
	spin_unlock_irqrestore(&tbl->lock, f);
	while (scan) {
		neighbour_t *victims[32];
		int timer_refs[32];
		int nv;

		spin_lock_irqsave(&tbl->lock, &f);
		nv = neigh_collect_locked(tbl, neigh_pick_stale, now, stale,
					  victims, timer_refs, 32);
		spin_unlock_irqrestore(&tbl->lock, f);
		for (int i = 0; i < nv; i++)
			neigh_unlinked_release(victims[i], timer_refs[i]);
		scan = nv == 32;
	}
}

static void neigh_gc_timer(net_timer_t *t)
{
	struct neigh_table *tbl =
		(struct neigh_table *)((char *)t -
				       __builtin_offsetof(struct neigh_table,
							  gc_timer));
	uint32_t period = tbl->parms.base_reachable_time_ms / 2;

	neigh_periodic_gc(tbl);
	if (period < 1000)
		period = 1000;
	net_timer_mod(&tbl->gc_timer, timer_ticks() + ms2t(period));
}

void neigh_table_init(struct neigh_table *tbl)
{
	neigh_pool_init();
	spinlock_init(&tbl->lock, tbl->id ? tbl->id : "neigh_table");
	for (int i = 0; i < NEIGH_HASH_SIZE; i++)
		tbl->hash[i] = NULL;
	tbl->entries = 0;
	tbl->last_flush = timer_ticks();
	mm_memset(&tbl->stats, 0, sizeof(tbl->stats));
	if (tbl->gc_thresh3 > NEIGH_POOL_SIZE)
		tbl->gc_thresh3 = NEIGH_POOL_SIZE;
	neigh_randomize_reachable(tbl);
	net_timer_setup(&tbl->gc_timer, neigh_gc_timer);
	net_timer_mod(&tbl->gc_timer,
		      timer_ticks() +
			      ms2t(tbl->parms.base_reachable_time_ms / 2 + 1));
}
