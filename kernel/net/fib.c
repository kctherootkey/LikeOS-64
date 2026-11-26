// LikeOS — IPv4 forwarding information base
//
// See kernel/net/fib.h.  Routes and exceptions live in static arrays (the
// table is changed from process and softirq context, looked up from every
// context including under protocol locks, so no allocation happens here).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/fib.h>
#include <kernel/net/dst.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

#define FIB_HASH_SIZE 64
#define EXC_HASH_SIZE 128

struct fib_entry {
	struct fib_entry *hnext;
	uint32_t dst, mask;
	uint8_t plen, type;
	uint16_t flags;
	uint32_t gw;
	net_device_t *dev;
	uint32_t metric;
	uint32_t mtu, advmss, hoplimit, prefsrc;
	struct dst_entry *dst_cache; // the output entry, one reference
	int in_use;
};

struct fib_exc {
	struct fib_exc *hnext;
	uint32_t daddr;
	uint32_t gw; // next hop of the route it was made for
	net_device_t *dev;
	uint32_t mtu;
	uint64_t expires; // tick
	struct dst_entry *dst; // one reference
	int in_use;
	/* The report was below FIB_MIN_PMTU: the path MTU is the minimum and
	 * datagrams go without DF, so a router can fragment them. */
	uint8_t locked;
};

static struct fib_entry fib_entries[FIB_MAX_ROUTES];
static struct fib_entry *fib_hash[33][FIB_HASH_SIZE];
static uint32_t plen_count[33];
static uint64_t plen_mask; // bit p: routes with prefix length p exist
static uint32_t fib_nroutes;
static volatile uint32_t genid = 1;

static struct fib_exc exc_pool[FIB_MAX_EXCEPTIONS];
static struct fib_exc *exc_hash[EXC_HASH_SIZE];
static uint32_t exc_count;
/* Per-destination generations (by exception hash bucket): bumped when a
 * destination's exception is made or lowered, so a sender that keeps the
 * route's shared entry for that destination looks it up again and finds
 * the exception -- without invalidating what every other sender keeps. */
static volatile uint32_t exc_genid[EXC_HASH_SIZE];

static spinlock_t fib_lock = SPINLOCK_INIT("fib");

uint32_t fib_genid(void)
{
	return __atomic_load_n(&genid, __ATOMIC_ACQUIRE);
}

void fib_genid_bump(void)
{
	uint32_t g = __atomic_add_fetch(&genid, 1, __ATOMIC_ACQ_REL);

	if (g == 0) // keep 0 unused
		__atomic_add_fetch(&genid, 1, __ATOMIC_ACQ_REL);
}

static inline uint32_t plen_to_mask(int plen)
{
	return plen ? 0xFFFFFFFFu << (32 - plen) : 0;
}

static int mask_to_plen(uint32_t mask)
{
	uint32_t inv = ~mask;

	int plen = 0;

	if (inv & (inv + 1))
		return -1; // not contiguous
	while (mask & 0x80000000u) {
		plen++;
		mask <<= 1;
	}
	return plen;
}

static inline uint32_t fib_hashfn(uint32_t key)
{
	key ^= key >> 16;
	key *= 0x7feb352dU;
	key ^= key >> 15;
	return key & (FIB_HASH_SIZE - 1);
}

static inline uint32_t exc_hashfn(uint32_t daddr)
{
	daddr ^= daddr >> 16;
	daddr *= 0x846ca68bU;
	daddr ^= daddr >> 16;
	return daddr & (EXC_HASH_SIZE - 1);
}

void fib_init(void)
{
	uint64_t f;

	spin_lock_irqsave(&fib_lock, &f);
	mm_memset(fib_entries, 0, sizeof(fib_entries));
	mm_memset(fib_hash, 0, sizeof(fib_hash));
	mm_memset(plen_count, 0, sizeof(plen_count));
	plen_mask = 0;
	fib_nroutes = 0;
	mm_memset(exc_pool, 0, sizeof(exc_pool));
	mm_memset(exc_hash, 0, sizeof(exc_hash));
	exc_count = 0;
	spin_unlock_irqrestore(&fib_lock, f);
}

static struct fib_entry *fib_find_exact_locked(uint32_t dst, int plen)
{
	for (struct fib_entry *e = fib_hash[plen][fib_hashfn(dst)]; e;
	     e = e->hnext)
		if (e->dst == dst && e->plen == plen)
			return e;
	return NULL;
}

static void fib_unlink_locked(struct fib_entry *e)
{
	struct fib_entry **pp = &fib_hash[e->plen][fib_hashfn(e->dst)];

	for (; *pp; pp = &(*pp)->hnext) {
		if (*pp == e) {
			*pp = e->hnext;
			break;
		}
	}
	e->hnext = NULL;
	if (--plen_count[e->plen] == 0)
		plen_mask &= ~(1ULL << e->plen);
	fib_nroutes--;
}

static void fib_fill(struct fib_entry *e, const struct fib_config *c,
		     int plen)
{
	e->dst = c->dst;
	e->mask = c->mask;
	e->plen = (uint8_t)plen;
	e->type = c->type ? c->type :
			    ((c->flags & RTF_REJECT) ? RTN_UNREACHABLE :
						       RTN_UNICAST);
	e->flags = c->flags;
	e->gw = c->gw;
	e->dev = c->dev;
	e->metric = c->metric;
	e->mtu = c->mtu;
	e->advmss = c->advmss;
	e->hoplimit = c->hoplimit;
	e->prefsrc = c->prefsrc;
}

int fib_table_insert(const struct fib_config *cfg)
{
	struct fib_entry *e, *slot = NULL;
	struct dst_entry *old_dst = NULL;
	int plen = mask_to_plen(cfg->mask);
	uint64_t f;

	if (plen < 0 || (cfg->dst & ~cfg->mask))
		return -EINVAL;
	spin_lock_irqsave(&fib_lock, &f);
	e = fib_find_exact_locked(cfg->dst, plen);
	if (e) {
		// The same prefix again: the new route replaces it.
		old_dst = e->dst_cache;
		e->dst_cache = NULL;
		fib_fill(e, cfg, plen);
		fib_genid_bump();
		spin_unlock_irqrestore(&fib_lock, f);
		if (old_dst) {
			dst_mark_obsolete(old_dst);
			dst_release(old_dst);
		}
		return 0;
	}
	for (int i = 0; i < FIB_MAX_ROUTES; i++) {
		if (!fib_entries[i].in_use) {
			slot = &fib_entries[i];
			break;
		}
	}
	if (!slot) {
		spin_unlock_irqrestore(&fib_lock, f);
		return -ENOSPC;
	}
	mm_memset(slot, 0, sizeof(*slot));
	fib_fill(slot, cfg, plen);
	slot->in_use = 1;
	{
		uint32_t h = fib_hashfn(cfg->dst);

		slot->hnext = fib_hash[plen][h];
		fib_hash[plen][h] = slot;
	}
	plen_count[plen]++;
	plen_mask |= 1ULL << plen;
	fib_nroutes++;
	fib_genid_bump();
	spin_unlock_irqrestore(&fib_lock, f);
	return 0;
}

int fib_table_delete(uint32_t dst, uint32_t mask, uint32_t gw)
{
	int plen = mask_to_plen(mask);
	struct fib_entry *e;
	struct dst_entry *old_dst;
	uint64_t f;

	if (plen < 0)
		return -ESRCH;
	spin_lock_irqsave(&fib_lock, &f);
	e = fib_find_exact_locked(dst & mask, plen);
	if (!e || (gw && e->gw != gw)) {
		spin_unlock_irqrestore(&fib_lock, f);
		return -ESRCH;
	}
	fib_unlink_locked(e);
	old_dst = e->dst_cache;
	e->dst_cache = NULL;
	e->in_use = 0;
	fib_genid_bump();
	spin_unlock_irqrestore(&fib_lock, f);
	if (old_dst) {
		dst_mark_obsolete(old_dst);
		dst_release(old_dst);
	}
	return 0;
}

/* Longest-prefix match; with `oif', only among the routes out of that
 * device (a sender bound to a device never leaves through another). */
static struct fib_entry *fib_lookup_oif_locked(uint32_t daddr,
					       const net_device_t *oif)
{
	uint64_t m = plen_mask;

	while (m) {
		int plen = 63 - __builtin_clzll(m);
		uint32_t key = daddr & plen_to_mask(plen);
		struct fib_entry *best = NULL;

		m &= ~(1ULL << plen);
		for (struct fib_entry *e = fib_hash[plen][fib_hashfn(key)]; e;
		     e = e->hnext) {
			if (e->dst != key || e->plen != plen ||
			    !(e->flags & RTF_UP))
				continue;
			if (oif && e->dev != oif)
				continue;
			if (!best || e->metric < best->metric)
				best = e;
		}
		if (best)
			return best;
	}
	return NULL;
}

static struct fib_entry *fib_lookup_locked(uint32_t daddr)
{
	return fib_lookup_oif_locked(daddr, NULL);
}

static void fib_result_fill(const struct fib_entry *e, struct fib_result *r)
{
	r->prefix = e->dst;
	r->mask = e->mask;
	r->plen = e->plen;
	r->type = e->type;
	r->flags = e->flags;
	r->gw = (e->flags & RTF_GATEWAY) ? e->gw : 0;
	r->dev = e->dev;
	r->metric = e->metric;
	r->mtu = e->mtu;
	r->advmss = e->advmss;
	r->hoplimit = e->hoplimit;
	r->prefsrc = e->prefsrc;
}

int fib_lookup(uint32_t daddr, struct fib_result *res)
{
	struct fib_entry *e;
	uint64_t f;

	spin_lock_irqsave(&fib_lock, &f);
	e = fib_lookup_locked(daddr);
	if (e)
		fib_result_fill(e, res);
	spin_unlock_irqrestore(&fib_lock, f);
	return e ? 0 : -ENETUNREACH;
}

// ---- exceptions ----

static struct fib_exc *exc_find_locked(uint32_t daddr)
{
	for (struct fib_exc *x = exc_hash[exc_hashfn(daddr)]; x; x = x->hnext)
		if (x->daddr == daddr)
			return x;
	return NULL;
}

/* Unlink `x'; its dst reference is returned for release after unlock. */
static struct dst_entry *exc_remove_locked(struct fib_exc *x)
{
	struct fib_exc **pp = &exc_hash[exc_hashfn(x->daddr)];
	struct dst_entry *d = x->dst;

	for (; *pp; pp = &(*pp)->hnext) {
		if (*pp == x) {
			*pp = x->hnext;
			break;
		}
	}
	x->hnext = NULL;
	x->dst = NULL;
	x->in_use = 0;
	exc_count--;
	if (d)
		dst_mark_obsolete(d);
	return d;
}

uint32_t fib_exc_genid(uint32_t daddr)
{
	return __atomic_load_n(&exc_genid[exc_hashfn(daddr)], __ATOMIC_ACQUIRE);
}

static void exc_genid_bump(uint32_t daddr)
{
	__atomic_add_fetch(&exc_genid[exc_hashfn(daddr)], 1, __ATOMIC_ACQ_REL);
}

static inline int exc_expired(const struct fib_exc *x, uint64_t now)
{
	return (int64_t)(now - x->expires) >= 0;
}

/* A dst for exception `x' made in the current generation (fib lock
 * held).  NULL when the pool is empty. */
static struct dst_entry *exc_dst_locked(struct fib_exc *x,
					const struct fib_entry *e,
					struct dst_entry **old)
{
	struct dst_entry *d = x->dst;

	if (d && dst_valid(d))
		return d;
	*old = d;
	x->dst = NULL;
	d = dst_alloc(x->dev, RTN_UNICAST);
	if (!d)
		return NULL;
	d->gateway = x->gw;
	d->mtu = x->mtu;
	d->expires = x->expires;
	d->flags = DST_HOST | (x->locked ? DST_MTU_LOCKED : 0);
	d->daddr = x->daddr;
	d->metric_mtu = e->mtu;
	d->advmss = e->advmss;
	d->hoplimit = e->hoplimit;
	d->prefsrc = e->prefsrc;
	x->dst = d;
	return d;
}

int fib_exception_set_pmtu(uint32_t daddr, uint32_t mtu)
{
	struct dst_entry *old = NULL, *junk = NULL, *stale = NULL;
	struct fib_entry *e;
	struct fib_exc *x;
	uint64_t now = timer_ticks();
	uint64_t f;
	int rc = 0;
	uint8_t locked = 0;

	if (mtu < FIB_MIN_PMTU) {
		mtu = FIB_MIN_PMTU;
		locked = 1;
	}
	spin_lock_irqsave(&fib_lock, &f);
	e = fib_lookup_locked(daddr);
	if (!e || e->type != RTN_UNICAST || !e->dev) {
		rc = -ENETUNREACH;
		goto out;
	}
	if (mtu >= e->dev->mtu || (e->mtu && mtu >= e->mtu))
		goto out; // not smaller than what the route allows anyway
	x = exc_find_locked(daddr);
	if (x && !exc_expired(x, now) && x->mtu <= mtu &&
	    (x->locked || !locked))
		goto out; // reports only ever lower the path MTU
	if (!x) {
		struct fib_exc *oldest = NULL;

		for (int i = 0; i < FIB_MAX_EXCEPTIONS; i++) {
			struct fib_exc *c = &exc_pool[i];

			if (!c->in_use) {
				x = c;
				break;
			}
			if (!oldest || (int64_t)(c->expires - oldest->expires) < 0)
				oldest = c;
		}
		if (!x) {
			junk = exc_remove_locked(oldest);
			x = oldest;
		}
		mm_memset(x, 0, sizeof(*x));
		x->daddr = daddr;
		x->in_use = 1;
		{
			uint32_t h = exc_hashfn(daddr);

			x->hnext = exc_hash[h];
			exc_hash[h] = x;
		}
		exc_count++;
	}
	x->gw = (e->flags & RTF_GATEWAY) ? e->gw : 0;
	x->dev = e->dev;
	if (!x->mtu || x->mtu > mtu || exc_expired(x, now)) {
		if (exc_expired(x, now))
			x->locked = 0;
		x->mtu = mtu;
	}
	x->locked = (uint8_t)(x->locked | locked);
	x->expires = now + net_ms_to_ticks(FIB_PMTU_EXPIRES_MS);
	old = x->dst;
	x->dst = NULL;
	if (old)
		dst_mark_obsolete(old);
	/* Senders keeping the route's entry for this destination look the
	 * route up again and find the exception; nobody else's entry
	 * changes (the old exception entry is obsolete above). */
	exc_genid_bump(daddr);
	(void)exc_dst_locked(x, e, &stale);
out:
	spin_unlock_irqrestore(&fib_lock, f);
	dst_release(old);
	dst_release(junk);
	dst_release(stale);
	return rc;
}

uint32_t fib_exception_get_pmtu(uint32_t daddr)
{
	struct fib_exc *x;
	uint32_t mtu = 0;
	uint64_t f;

	spin_lock_irqsave(&fib_lock, &f);
	x = exc_find_locked(daddr);
	if (x && !exc_expired(x, timer_ticks()))
		mtu = x->mtu;
	spin_unlock_irqrestore(&fib_lock, f);
	return mtu;
}

void fib_exceptions_gc(void)
{
	uint64_t now = timer_ticks();

	for (;;) {
		struct dst_entry *junk[16];
		int n = 0;
		uint64_t f;

		spin_lock_irqsave(&fib_lock, &f);
		for (int i = 0; i < FIB_MAX_EXCEPTIONS && n < 16; i++) {
			struct fib_exc *x = &exc_pool[i];

			if (x->in_use && exc_expired(x, now))
				junk[n++] = exc_remove_locked(x);
		}
		spin_unlock_irqrestore(&fib_lock, f);
		for (int i = 0; i < n; i++)
			dst_release(junk[i]);
		if (n < 16)
			return;
	}
}

void fib_exceptions_flush(void)
{
	for (;;) {
		struct dst_entry *junk[16];
		int n = 0;
		uint64_t f;

		spin_lock_irqsave(&fib_lock, &f);
		for (int i = 0; i < FIB_MAX_EXCEPTIONS && n < 16; i++) {
			struct fib_exc *x = &exc_pool[i];

			if (x->in_use)
				junk[n++] = exc_remove_locked(x);
		}
		spin_unlock_irqrestore(&fib_lock, f);
		for (int i = 0; i < n; i++)
			dst_release(junk[i]);
		if (n < 16)
			return;
	}
}

uint32_t fib_exception_count(void)
{
	return __atomic_load_n(&exc_count, __ATOMIC_RELAXED);
}

// ---- output ----

int fib_route_output(uint32_t daddr, const net_device_t *oif,
		     struct dst_entry **out)
{
	struct dst_entry *d, *old = NULL, *junk = NULL;
	struct fib_entry *e;
	struct fib_exc *x;
	uint64_t now = timer_ticks();
	uint64_t f;
	int rc = 0;

	*out = NULL;
	spin_lock_irqsave(&fib_lock, &f);
	e = fib_lookup_oif_locked(daddr, oif);
	if (!e) {
		rc = -ENETUNREACH;
		goto out;
	}
	switch (e->type) {
	case RTN_UNREACHABLE:
		rc = -EHOSTUNREACH;
		goto out;
	case RTN_PROHIBIT:
		rc = -EACCES;
		goto out;
	case RTN_BLACKHOLE:
		rc = -EINVAL;
		goto out;
	default:
		break;
	}
	if (!e->dev) {
		rc = -ENETUNREACH;
		goto out;
	}
	x = exc_find_locked(daddr);
	if (x) {
		uint32_t gw = (e->flags & RTF_GATEWAY) ? e->gw : 0;

		if (exc_expired(x, now) ||
		    (!oif && (x->dev != e->dev || x->gw != gw))) {
			// stale, or the route changed under it
			junk = exc_remove_locked(x);
		} else if (x->dev != e->dev || x->gw != gw) {
			/* Made for the destination's path through another
			 * device or next hop: not this sender's path. */
		} else {
			d = exc_dst_locked(x, e, &old);
			if (d) {
				d->lastuse = now;
				*out = dst_hold(d);
				goto out;
			}
		}
	}
	d = e->dst_cache;
	if (!d || !dst_valid(d)) {
		old = d;
		e->dst_cache = NULL;
		d = dst_alloc(e->dev, e->type);
		if (!d) {
			rc = -ENOBUFS;
			goto out;
		}
		d->gateway = (e->flags & RTF_GATEWAY) ? e->gw : 0;
		d->metric_mtu = e->mtu;
		d->advmss = e->advmss;
		d->hoplimit = e->hoplimit;
		d->prefsrc = e->prefsrc;
		e->dst_cache = d;
	}
	d->lastuse = now;
	*out = dst_hold(d);
out:
	spin_unlock_irqrestore(&fib_lock, f);
	dst_release(old);
	dst_release(junk);
	return rc;
}

// ---- dump ----

int fib_dump(struct net_route_info *out, int max)
{
	int count = 0;
	uint64_t f;

	spin_lock_irqsave(&fib_lock, &f);
	for (int i = 0; i < FIB_MAX_ROUTES && count < max; i++) {
		const struct fib_entry *e = &fib_entries[i];
		net_route_info_t *r = &out[count];

		if (!e->in_use)
			continue;
		r->dst_net = e->dst;
		r->netmask = e->mask;
		r->gateway = e->gw;
		r->flags = e->flags;
		r->metric = (uint16_t)e->metric;
		if (e->dev && e->dev->name) {
			const char *n = e->dev->name;
			int j = 0;

			while (n[j] && j < 15) {
				r->dev_name[j] = n[j];
				j++;
			}
			r->dev_name[j] = '\0';
		} else {
			r->dev_name[0] = '*';
			r->dev_name[1] = '\0';
		}
		count++;
	}
	spin_unlock_irqrestore(&fib_lock, f);
	return count;
}

uint32_t fib_route_count(void)
{
	return __atomic_load_n(&fib_nroutes, __ATOMIC_RELAXED);
}
