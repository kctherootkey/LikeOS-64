// LikeOS — destination cache entries
//
// See kernel/net/dst.h.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/dst.h>
#include <kernel/net/fib.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

#define DST_POOL_SIZE 1024

static struct dst_entry dst_pool[DST_POOL_SIZE];
static struct dst_entry *dst_free_list;
static uint32_t dst_nfree;
static spinlock_t dst_pool_lock = SPINLOCK_INIT("dst_pool");
static volatile uint64_t st_allocs, st_frees, st_fail;

void dst_init(void)
{
	dst_free_list = NULL;
	for (int i = DST_POOL_SIZE - 1; i >= 0; i--) {
		dst_pool[i].free_next = dst_free_list;
		dst_free_list = &dst_pool[i];
	}
	dst_nfree = DST_POOL_SIZE;
}

struct dst_entry *dst_alloc(net_device_t *dev, uint8_t type)
{
	struct dst_entry *d;
	uint64_t f;

	spin_lock_irqsave(&dst_pool_lock, &f);
	d = dst_free_list;
	if (d) {
		dst_free_list = d->free_next;
		dst_nfree--;
	}
	spin_unlock_irqrestore(&dst_pool_lock, f);
	if (!d) {
		__atomic_fetch_add(&st_fail, 1, __ATOMIC_RELAXED);
		return NULL;
	}
	mm_memset(d, 0, sizeof(*d));
	refcount_set(&d->refcnt, 1);
	d->dev = dev;
	d->type = type;
	d->genid = fib_genid();
	d->lastuse = timer_ticks();
	__atomic_fetch_add(&st_allocs, 1, __ATOMIC_RELAXED);
	return d;
}

void dst_release(struct dst_entry *dst)
{
	uint64_t f;

	if (!dst || !refcount_dec_and_test(&dst->refcnt))
		return;
	dst->dev = NULL;
	spin_lock_irqsave(&dst_pool_lock, &f);
	dst->free_next = dst_free_list;
	dst_free_list = dst;
	dst_nfree++;
	spin_unlock_irqrestore(&dst_pool_lock, f);
	__atomic_fetch_add(&st_frees, 1, __ATOMIC_RELAXED);
}

int dst_valid(const struct dst_entry *dst)
{
	if (__atomic_load_n(&dst->obsolete, __ATOMIC_ACQUIRE))
		return 0;
	if (dst->genid != fib_genid())
		return 0;
	if (dst->expires && (int64_t)(timer_ticks() - dst->expires) >= 0)
		return 0;
	return 1;
}

uint32_t dst_mtu(const struct dst_entry *dst)
{
	uint32_t mtu = dst->dev ? dst->dev->mtu : NET_MTU_DEFAULT;

	if (dst->metric_mtu && dst->metric_mtu < mtu)
		mtu = dst->metric_mtu;
	if (dst->mtu && dst->mtu < mtu &&
	    (!dst->expires || (int64_t)(timer_ticks() - dst->expires) < 0))
		mtu = dst->mtu;
	return mtu;
}

uint32_t dst_advmss(const struct dst_entry *dst)
{
	uint32_t mtu;

	if (dst->advmss)
		return dst->advmss;
	mtu = dst_mtu(dst);
	return mtu > 40 + 256 ? mtu - 40 : 256;
}

uint32_t dst_hoplimit(const struct dst_entry *dst)
{
	return dst->hoplimit ? dst->hoplimit : 64;
}

struct dst_entry *dst_cache_get(struct dst_cache *c)
{
	struct dst_entry *d = c->dst;

	if (!d)
		return NULL;
	if (!dst_valid(d) || c->exc_genid != fib_exc_genid(c->daddr)) {
		c->dst = NULL;
		dst_release(d);
		return NULL;
	}
	return dst_hold(d);
}

void dst_cache_set(struct dst_cache *c, struct dst_entry *dst, uint32_t daddr)
{
	struct dst_entry *old = c->dst;

	uint32_t gen = fib_exc_genid(daddr);

	/* A route's shared entry looked up just before an exception for
	 * this destination was made: kept, but the next dst_cache_get()
	 * looks the route up again and finds the exception. */
	if (dst && !(dst->flags & DST_HOST) && dst->type == RTN_UNICAST &&
	    fib_exception_get_pmtu(daddr))
		gen--;
	c->exc_genid = gen;
	c->dst = dst_hold(dst);
	c->daddr = daddr;
	dst_release(old);
}

void dst_cache_reset(struct dst_cache *c)
{
	struct dst_entry *old = c->dst;

	c->dst = NULL;
	dst_release(old);
}

void dst_get_stats(struct dst_stats *s)
{
	uint64_t f;

	spin_lock_irqsave(&dst_pool_lock, &f);
	s->pool_free = dst_nfree;
	spin_unlock_irqrestore(&dst_pool_lock, f);
	s->pool_total = DST_POOL_SIZE;
	s->allocs = st_allocs;
	s->frees = st_frees;
	s->alloc_fail = st_fail;
}
