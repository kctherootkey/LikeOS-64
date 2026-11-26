// LikeOS — IPv4 forwarding information base (routing table)
//
// Routes live in one hash table per prefix length (0..32).  A lookup tries
// the populated lengths from the longest down and returns the first
// prefix that contains the address -- the longest match -- choosing the
// lowest metric among routes for the same prefix.  Each route carries its
// metrics (MTU, advertised MSS, hop limit) and keeps the dst_entry it
// hands to senders.  Every change to the table bumps the FIB generation,
// which invalidates every dst_entry handed out before.
//
// Path-MTU exceptions: a destination for which an ICMP "fragmentation
// needed" reported a smaller MTU gets an entry of its own (with its own
// dst_entry) that expires after FIB_PMTU_EXPIRES_MS; while it lives, the
// output route for that destination carries the smaller MTU.  Making or
// lowering an exception does not change the FIB generation: it bumps the
// destination's own generation (fib_exc_genid), which only the socket
// route caches kept for that destination check (struct dst_cache).
//
// Local and broadcast destinations need no table entry: one of our
// interface addresses routes to the loopback device (RTN_LOCAL), the
// limited and the subnet broadcast address to the interface
// (RTN_BROADCAST); see ip_route_output() in route.c.
//
// Lock order: fib lock -> dst pool lock.  Never held across a call out.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_FIB_H
#define LIKEOS_FIB_H

#include <kernel/uapi/types.h>
#include <kernel/net/dst.h>

struct net_device;
typedef struct net_device net_device_t;

#define FIB_MAX_ROUTES 256
#define FIB_MAX_EXCEPTIONS 256
#define FIB_PMTU_EXPIRES_MS (600u * 1000u)
#define FIB_MIN_PMTU 552 // the smallest path MTU a report may set

struct fib_config {
	uint32_t dst; // network, host order (bits outside the mask zero)
	uint32_t mask;
	uint32_t gw; // 0: directly connected
	net_device_t *dev;
	uint32_t metric;
	uint16_t flags; // RTF_*
	uint8_t type; // RTN_*
	uint8_t _pad;
	uint32_t mtu, advmss, hoplimit; // metrics, 0: none
	uint32_t prefsrc;
};

struct fib_result {
	uint32_t prefix, mask;
	uint8_t plen, type;
	uint16_t flags;
	uint32_t gw;
	net_device_t *dev;
	uint32_t metric;
	uint32_t mtu, advmss, hoplimit, prefsrc;
};

void fib_init(void);
// Add, or replace the route for the same prefix (dst/mask).  0, -EINVAL
// (mask not contiguous, host bits set), -ENOSPC.
int fib_table_insert(const struct fib_config *cfg);
// Remove the route for dst/mask (and gw, when nonzero).  0 or -ESRCH.
int fib_table_delete(uint32_t dst, uint32_t mask, uint32_t gw);
// Longest-prefix match.  0, or -ENETUNREACH when nothing matches.
int fib_lookup(uint32_t daddr, struct fib_result *res);
// The output entry for `daddr' through the matching route, held (the
// route's own, or the destination's PMTU exception's).  With `oif' only
// routes out of that device match.  0, -ENETUNREACH,
// -EHOSTUNREACH/-EACCES/-EINVAL for reject routes, -ENOBUFS.
int fib_route_output(uint32_t daddr, const net_device_t *oif,
		     struct dst_entry **out);

uint32_t fib_genid(void);
void fib_genid_bump(void); // invalidate every dst handed out
// Generation of `daddr''s path-MTU exception (shared by a hash bucket).
uint32_t fib_exc_genid(uint32_t daddr);

// Path-MTU exceptions.
int fib_exception_set_pmtu(uint32_t daddr, uint32_t mtu);
uint32_t fib_exception_get_pmtu(uint32_t daddr); // 0: none
void fib_exceptions_gc(void); // drop expired ones
void fib_exceptions_flush(void);
uint32_t fib_exception_count(void);

// Table snapshot for netstat/route, in insertion order.
struct net_route_info;
int fib_dump(struct net_route_info *out, int max);
uint32_t fib_route_count(void);

#endif // LIKEOS_FIB_H
