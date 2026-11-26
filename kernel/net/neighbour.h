// LikeOS — neighbour subsystem (next-hop address resolution)
//
// A neighbour is the layer-2 address of an on-link IPv4 next hop on one
// device, with the Neighbour Unreachability Detection state machine that
// decides whether it can be trusted:
//
//   NONE ---send---> INCOMPLETE --reply--> REACHABLE --reachable_time-->
//   STALE --send--> DELAY --delay_first_probe_time, not confirmed-->
//   PROBE --probes exhausted--> FAILED
//
//   * INCOMPLETE: resolution in progress; packets wait in the entry's
//     queue, up to `queue_len_bytes' of them (the oldest is dropped
//     beyond it); a broadcast request every retrans_time, FAILED after
//     mcast_probes.
//   * REACHABLE: confirmed within reachable_time (randomized around
//     base_reachable_time) by a reply or by an upper layer (TCP: an ACK
//     that advanced the window, neigh_confirm()).
//   * STALE: usable, but not confirmed lately.  The next packet sent moves
//     it to DELAY.
//   * DELAY: give the upper layers delay_first_probe_time to confirm;
//     then PROBE.
//   * PROBE: unicast requests to the cached address every retrans_time;
//     FAILED after ucast_probes.
//   * FAILED: unreachable; the queued packets are reported back
//     (ICMP host unreachable to their local sender).  The next packet
//     starts a new resolution.
//   * NOARP: no resolution needed (broadcast, multicast, loopback).
//   * PERMANENT: set by the administrator; never changes on its own.
//
// Entries are learned only from address resolution itself (a request for
// one of our addresses creates or refreshes its sender; replies and other
// requests refresh entries that exist), never from ordinary IP traffic.
//
// Lifetime: an entry is reference counted.  The hash table holds one
// reference while the entry is linked; a lookup returns another
// (neigh_release() when done); a pending state timer holds one, dropped by
// its callback or when it is deleted.  The memory (a static pool, so
// entries can be created in any context) is returned when the count drops
// to zero; the queued packets are freed then.  Unused entries are removed
// by the table's garbage collector (gc_thresh1/2/3, gc_staletime).
//
// Lock order: tbl->lock -> n->lock -> (net timer wheel lock).  Neither is
// held across a transmit, a solicitation or an error report.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_NEIGHBOUR_H
#define LIKEOS_NEIGHBOUR_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/bug.h>
#include <kernel/net/skb.h>
#include <kernel/net/net_timer.h>
#include <kernel/ke/timer.h>

struct net_device;
typedef struct net_device net_device_t;

#define NUD_NONE 0x00
#define NUD_INCOMPLETE 0x01
#define NUD_REACHABLE 0x02
#define NUD_STALE 0x04
#define NUD_DELAY 0x08
#define NUD_PROBE 0x10
#define NUD_FAILED 0x20
#define NUD_NOARP 0x40
#define NUD_PERMANENT 0x80

#define NUD_IN_TIMER (NUD_INCOMPLETE | NUD_REACHABLE | NUD_DELAY | NUD_PROBE)
#define NUD_VALID                                                     \
	(NUD_PERMANENT | NUD_NOARP | NUD_REACHABLE | NUD_PROBE | NUD_STALE | \
	 NUD_DELAY)
#define NUD_CONNECTED (NUD_PERMANENT | NUD_NOARP | NUD_REACHABLE)

// neigh_update() flags
#define NEIGH_UPDATE_F_OVERRIDE 0x1 // a different address replaces ours
#define NEIGH_UPDATE_F_WEAK_OVERRIDE 0x2 // ... only by making us STALE
#define NEIGH_UPDATE_F_ADMIN 0x80000000 // administrator: may touch
	// PERMANENT/NOARP entries

#define NEIGH_HASH_SIZE 256
#define NEIGH_HALEN 6

struct neigh_parms {
	uint32_t base_reachable_time_ms; // 30 s
	uint32_t retrans_time_ms; // 1 s
	uint32_t delay_probe_time_ms; // 5 s
	uint32_t gc_staletime_ms; // 60 s
	uint32_t locktime_ms; // 1 s: replies within it do not override
	uint32_t ucast_probes; // 3
	uint32_t mcast_probes; // 3
	uint32_t app_probes; // 0
	uint32_t mcast_reprobes; // 0
	/* Packets waiting on an unresolved entry, by memory (truesize): the
	 * oldest is dropped beyond it. */
	uint32_t queue_len_bytes; // 212992 (the default socket buffer)
};

struct neigh_table;

typedef struct neighbour {
	struct neighbour *hnext; // hash chain (tbl->lock)
	struct neigh_table *tbl;
	net_device_t *dev;
	uint32_t key; // IPv4 address, host order
	refcount_t refcnt;
	spinlock_t lock; // the fields below
	uint8_t nud_state;
	uint8_t dead; // unlinked from the table
	uint8_t ha[NEIGH_HALEN]; // hardware address
	uint32_t probes; // solicitations sent in this state
	uint64_t confirmed; // tick reachability was last confirmed
	uint64_t updated; // tick the state or address last changed
	uint64_t used; // tick a packet last needed the entry
	net_timer_t timer;
	skb_queue_t arp_queue; // packets waiting for resolution
	uint32_t arp_queue_bytes; // their truesize (n->lock)
	struct neighbour *free_next;
} neighbour_t;

struct neigh_ops {
	// Send a solicitation for `n' with source address `saddr' (0: any);
	// unicast to n->ha when `unicast'.  No lock held.
	void (*solicit)(neighbour_t *n, uint32_t saddr, int unicast,
			const uint8_t ha[NEIGH_HALEN]);
	// A queued packet could not be delivered (FAILED).  No lock held;
	// the callee frees the skb.
	void (*error_report)(neighbour_t *n, sk_buff_t *skb);
	// Set up a new entry (NOARP for broadcast/multicast, ...); n->lock
	// is not needed (the entry is not visible yet).
	void (*constructor)(neighbour_t *n);
};

struct neigh_statistics {
	uint64_t allocs, destroys;
	uint64_t lookups, hits;
	uint64_t res_failed; // FAILED transitions
	uint64_t unres_discards; // packets dropped from a full queue
	uint64_t forced_gc_runs, periodic_gc_runs;
	uint64_t table_fulls;
	uint64_t rcv_probes_ucast, rcv_probes_mcast;
	uint64_t solicits;
};

struct neigh_table {
	const char *id;
	struct neigh_parms parms;
	const struct neigh_ops *ops;
	uint32_t gc_thresh1, gc_thresh2, gc_thresh3;
	uint32_t gc_interval_ms;
	uint32_t entries; // tbl->lock
	uint32_t reachable_time_ms; // randomized base_reachable_time
	uint64_t last_flush; // tick of the last forced gc
	uint64_t last_rand; // tick reachable_time was last drawn
	spinlock_t lock;
	neighbour_t *hash[NEIGH_HASH_SIZE];
	net_timer_t gc_timer;
	struct neigh_statistics stats;
};

void neigh_table_init(struct neigh_table *tbl);

static inline void neigh_hold(neighbour_t *n)
{
	refcount_inc(&n->refcnt);
}

void neigh_release(neighbour_t *n);

// Existing entry for (key, dev), held; NULL if none.
neighbour_t *neigh_lookup(struct neigh_table *tbl, uint32_t key,
			  net_device_t *dev);
// The same, creating a NONE entry when there is none (when `creat').
// NULL when the table is full even after garbage collection.
neighbour_t *__neigh_lookup(struct neigh_table *tbl, uint32_t key,
			    net_device_t *dev, int creat);

// Change state/address under the NUD rules (see neighbour.c).  `lladdr'
// NULL keeps the current address.  Returns 0, or -1 when the update was
// refused (no address known, PERMANENT entry without ADMIN, ...).
int neigh_update(neighbour_t *n, const uint8_t *lladdr, uint8_t new_state,
		 uint32_t flags);

// A packet needs the entry: kicks resolution or DELAY as the state asks.
// Returns 0 when the packet can be sent now with n->ha (the skb is still
// the caller's), 1 when the skb was queued or dropped (consumed).  `skb'
// may be NULL (just start resolving).
int neigh_event_send(neighbour_t *n, sk_buff_t *skb);

// Reachability confirmed by an upper layer.
static inline void neigh_confirm(neighbour_t *n)
{
	uint64_t now = timer_ticks();

	if (__atomic_load_n(&n->confirmed, __ATOMIC_RELAXED) != now)
		__atomic_store_n(&n->confirmed, now, __ATOMIC_RELAXED);
}

// Send an IPv4 datagram (skb->data at the IP header, skb->dev set) to the
// neighbour: Ethernet header and dev_queue_xmit() when the address is
// usable, queued for resolution otherwise.  Consumes the skb.  Returns 0
// (sent or queued) or a negative value (dropped).
int neigh_output(neighbour_t *n, sk_buff_t *skb);

// A request for one of our addresses from (key, lladdr): create or
// refresh the sender's entry (STALE, overriding).  Returns it held.
neighbour_t *neigh_event_ns(struct neigh_table *tbl, const uint8_t *lladdr,
			    uint32_t key, net_device_t *dev);

// Remove (key, dev) -- every device when dev is NULL.  Returns the count.
int neigh_delete(struct neigh_table *tbl, uint32_t key, net_device_t *dev);
// Remove every entry of `dev' (PERMANENT ones too unless skip_perm).
void neigh_flush_dev(struct neigh_table *tbl, net_device_t *dev,
		     int skip_perm);

// Copy at most `max' entries that are in one of `states', after skipping
// the first `skip' of them (a snapshot; for chunked dumps).
struct neigh_dump_entry {
	uint32_t key;
	uint8_t ha[NEIGH_HALEN];
	uint8_t nud_state;
	uint8_t _pad;
	net_device_t *dev;
};
int neigh_dump(struct neigh_table *tbl, struct neigh_dump_entry *out,
	       int max, uint8_t states, int skip);

// Run the periodic garbage collection now (tests); normally its timer.
void neigh_periodic_gc(struct neigh_table *tbl);
// Forced collection as done when the table is above gc_thresh2/3.
int neigh_forced_gc(struct neigh_table *tbl);

#endif // LIKEOS_NEIGHBOUR_H
