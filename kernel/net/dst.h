// LikeOS — destination cache entries
//
// A dst_entry is the result of an output route lookup that a sender can
// keep: the device, the next hop, the route type and the path metrics
// (MTU, advertised MSS, hop limit).  Each route of the FIB keeps one for
// the destinations it serves; a path-MTU exception (ICMP fragmentation
// needed) gets one of its own for its single destination.
//
// Lifetime: reference counted (dst_hold/dst_release).  An entry becomes
// invalid when the routing table changes (every change bumps the FIB
// generation; an entry records the generation it was made in), when it is
// explicitly marked obsolete, or when it expires (exceptions, 600 s).  A
// holder checks with dst_check() before using a kept entry and looks the
// route up again when it fails.  The memory is a static pool (entries are
// made in any context, under protocol locks too); an entry goes back to it
// when its last reference is dropped.
//
// Socket caches (sk_dst_cache): struct dst_cache keeps one entry for a
// socket and the destination it is used for; dst_cache_get() returns it
// held while it is valid and no path-MTU exception for that destination
// was made or changed since it was stored.  The caller serialises access
// to the cache (the socket's lock).
//
// Lock order: the pool lock is a leaf (taken under the FIB lock).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_DST_H
#define LIKEOS_DST_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>

struct net_device;
typedef struct net_device net_device_t;

// Route types (rtm_type)
#define RTN_UNSPEC 0
#define RTN_UNICAST 1
#define RTN_LOCAL 2 // one of our addresses: delivered through loopback
#define RTN_BROADCAST 3
#define RTN_ANYCAST 4
#define RTN_MULTICAST 5
#define RTN_BLACKHOLE 6 // silently discard
#define RTN_UNREACHABLE 7 // EHOSTUNREACH
#define RTN_PROHIBIT 8 // EACCES

#define DST_OBSOLETE_NONE 0
#define DST_OBSOLETE_KILL 2 // explicitly invalidated

#define DST_HOST 0x01 // for one destination (an exception)
#define DST_MTU_LOCKED 0x02 // path MTU at the floor: send without DF

struct dst_entry {
	refcount_t refcnt;
	volatile int obsolete;
	net_device_t *dev;
	uint32_t gateway; // next hop; 0: the destination itself (on-link)
	uint32_t prefsrc; // preferred source address, 0: the device's
	uint8_t type; // RTN_*
	uint8_t flags; // DST_*
	uint16_t _pad;
	int error; // reject types: the errno a sender gets (negative)
	uint32_t mtu; // path MTU, 0: the device's (or the route metric)
	uint32_t metric_mtu; // route metric, 0: none
	uint32_t advmss; // route metric, 0: derived from the MTU
	uint32_t hoplimit; // route metric, 0: default (64)
	uint32_t genid; // FIB generation it belongs to
	uint32_t daddr; // DST_HOST: its destination
	uint64_t expires; // tick, 0: never
	uint64_t lastuse; // tick
	struct dst_entry *free_next;
};

void dst_init(void);
// A new entry (refcount 1) for `dev'; NULL when the pool is empty.
struct dst_entry *dst_alloc(net_device_t *dev, uint8_t type);

static inline struct dst_entry *dst_hold(struct dst_entry *dst)
{
	if (dst)
		refcount_inc(&dst->refcnt);
	return dst;
}

void dst_release(struct dst_entry *dst);

// The current FIB generation, and a destination's exception generation
// (fib.c).
uint32_t fib_genid(void);
uint32_t fib_exc_genid(uint32_t daddr);

// 1 while the entry may be used: not obsolete, made in the current FIB
// generation, not expired.
int dst_valid(const struct dst_entry *dst);

// The kept entry itself when valid, NULL otherwise (the caller still owns
// its reference either way and drops it when it gets NULL).
static inline struct dst_entry *dst_check(struct dst_entry *dst)
{
	return dst && dst_valid(dst) ? dst : NULL;
}

static inline void dst_mark_obsolete(struct dst_entry *dst)
{
	__atomic_store_n(&dst->obsolete, DST_OBSOLETE_KILL, __ATOMIC_RELEASE);
}

// The next hop towards `daddr' over this entry.
static inline uint32_t rt_nexthop(const struct dst_entry *dst, uint32_t daddr)
{
	return dst->gateway ? dst->gateway : daddr;
}

// Path MTU: the exception's, else the route metric, else the device's.
uint32_t dst_mtu(const struct dst_entry *dst);
// Advertised MSS: the route metric, else MTU - 40.
uint32_t dst_advmss(const struct dst_entry *dst);
// TTL for unicast: the route metric, else 64.
uint32_t dst_hoplimit(const struct dst_entry *dst);

// Socket route cache.
struct dst_cache {
	struct dst_entry *dst;
	uint32_t daddr;
	uint32_t exc_genid; // fib_exc_genid(daddr) when stored
};

struct dst_entry *dst_cache_get(struct dst_cache *c);
void dst_cache_set(struct dst_cache *c, struct dst_entry *dst,
		   uint32_t daddr);
void dst_cache_reset(struct dst_cache *c);

struct dst_stats {
	uint32_t pool_free, pool_total;
	uint64_t allocs, frees, alloc_fail;
};
void dst_get_stats(struct dst_stats *s);

#endif // LIKEOS_DST_H
