// LikeOS — Socket Buffer (sk_buff)
//
// A reference-counted, ownership-transferring packet buffer used end-to-end
// by the network stack.
//
// Layout
// ------
// An skb is a HEADER (struct sk_buff: pointers, metadata, control block)
// and a DATA area it points into:
//
//     head            data               tail            end
//      |<- headroom ->|<------ len ------>|<- tailroom ->|
//
// The data area is a "slot": one allocation holding an embedded sk_buff
// header, the skb_shared_info that counts how many headers use the data,
// and the buffer itself.  skb_alloc() returns the slot's embedded header.
// skb_clone() hands out a second header (from the clone-header pool) that
// points at the SAME data, so the data lives until its last header is gone
// (shinfo->dataref).  A cloned buffer is read-only: whoever wants to write
// into it takes skb_unshare() / skb_copy() first.
//
// Memory
// ------
// Slots come from two general size classes, small (a full Ethernet frame
// plus headroom) and jumbo (a full IPv4 datagram plus headroom), and a few
// fitted classes in between that only skb_alloc_fit() hands out (dynamic
// memory only, see below).  Each general class has
// a static pool -- the fast path, usable from any context including hard
// interrupts, because taking a slot is one swing of a free-list pointer
// under a short lock -- and a dynamic part built from slab memory when the
// static pool runs dry.  The slab allocator may never be called with
// interrupts disabled (a free can trigger a TLB-shootdown IPI that a CPU
// spinning with interrupts off cannot answer), so dynamic slots are:
//   * allocated in the caller's context only when interrupts are enabled
//     (every lock in the network stack is taken with interrupts off, so an
//     interrupts-enabled caller holds none of them);
//   * otherwise taken from a RESERVE of pre-allocated dynamic slots that
//     the network housekeeping softirq refills (process context,
//     interrupts on, no locks) whenever it runs low;
//   * never freed where they are released: a released dynamic slot goes
//     back onto the reserve, and the housekeeping softirq gives memory
//     beyond the reserve's high-water mark back to the slab allocator.
// A fitted class has no static pool and is never refilled ahead of use:
// its reserve holds only slots that were released, and the housekeeping
// softirq gives them back once the class has not been used for a while.
// All dynamic memory is accounted globally and capped (SKB_DYN_LIMIT).
//
// Ownership rules
// ---------------
//   * skb_alloc() returns a new skb with one reference (the caller's).
//   * Passing an skb to a queueing or transmit function transfers that
//     reference; the caller must not touch the skb afterwards.
//   * skb_get() takes another reference on the SAME header.
//   * skb_put() drops a reference (the name this stack has always used for
//     the release; skb_free() is the same call).  The header goes away at
//     zero, the data when its last header is gone.
//   * skb_append() adds bytes at the tail (the "put" of other stacks),
//     skb_push() at the head, skb_pull() removes from the head,
//     skb_trim() shortens the tail.
//
// Lock order: skb_queue_t locks and the pool locks are leaves.  No lock is
// ever held across a call into another layer.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_SKB_H
#define LIKEOS_SKB_H

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h> // spinlock_t
#include <kernel/uapi/bug.h>
#include <kernel/net/rbtree.h>

struct net_device;
typedef struct net_device net_device_t;

#define SKB_HEADROOM 128 // reserved at front for header prepends
/* A small slot must hold a maximum Ethernet frame (1518) PLUS the headroom
 * every skb reserves, because skb_alloc() sizes its request as payload +
 * SKB_HEADROOM.  At 1536 it did not: a full-size received frame (1514 +
 * 128) fell through to the JUMBO pool, whose 128 slots were the whole
 * receive capacity of the machine for full-MTU segments.  16-aligned. */
#define SKB_SMALL_DATA (1536 + SKB_HEADROOM)
/* A jumbo slot holds a full 65535-byte IPv4 datagram plus the headroom. */
#define SKB_JUMBO_DATA (65536 + SKB_HEADROOM)
/* Largest payload skb_alloc() accepts. */
#define SKB_MAX_ALLOC (SKB_JUMBO_DATA - SKB_HEADROOM)

/* Size of the protocol control block (skb->cb): private scratch space of
 * the layer that currently owns the skb.  Every layer may overwrite it. */
#define SKB_CB_SIZE 48

// skb->ip_summed: what is known about the transport checksum.
#define CHECKSUM_NONE 0 // nothing known; software verifies
#define CHECKSUM_UNNECESSARY 1 // verified (hardware or an earlier layer)
#define CHECKSUM_COMPLETE 2 // skb->csum is the ones' complement sum of
	// the packet from the network header on
#define CHECKSUM_PARTIAL 3 // TX: checksum to be finished at csum_start +
	// csum_offset (offload; not used by drivers yet)

// skb->pkt_type: how the frame was addressed at layer 2.
#define PACKET_HOST 0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define PACKET_OUTGOING 4
#define PACKET_LOOPBACK 5

#define SKB_HDR_UNSET 0xFFFFu // network/transport/mac header not set

// Size classes (skb_shared_info.cls).
#define SKB_CLS_SMALL 0
#define SKB_CLS_JUMBO 1
/* Fitted classes between the two, of 1, 2, 3, 4, 6, 8 and 12 pages per
 * slot: dynamic memory only (no static pool), handed out by
 * skb_alloc_fit() alone -- for a buffer that holds what it carries for a
 * while (a merged received segment) and should be charged about that,
 * not a whole jumbo slot. */
#define SKB_CLS_FIT0 2
#define SKB_NR_FIT 7
#define SKB_NR_CLS (SKB_CLS_FIT0 + SKB_NR_FIT)

struct skb_slot;

/* Shared state of one data area, used by every header that points at it. */
typedef struct skb_shared_info {
	volatile uint32_t dataref; // headers using this data
	uint8_t cls; // SKB_CLS_*
	uint8_t dynamic; // slab-backed (1) or static pool (0)
	uint16_t gso_segs; // segments merged into this one (GRO), 0/1 = one
	uint16_t gso_size; // payload size of each merged segment (GRO)
	uint16_t _pad;
	uint32_t size; // bytes of the data area
	struct skb_slot *slot; // the allocation this lives in
} skb_shared_info_t;

typedef struct sk_buff {
	union {
		struct {
			struct sk_buff *next; // queue linkage (owned by the queue)
			struct sk_buff *prev;
		};
		/* ... or the node of an ordered tree (TCP's out-of-order
		 * queue): an skb is on one queue or one tree at a time. */
		struct rb_node rbnode;
	};
	/* TCP's list of its segments in flight in the order they were (last)
	 * sent, beside the queue linkage (tcp_recovery.c: RACK walks it).
	 * Owned by TCP while the skb is on its retransmit queue; NULL on any
	 * other skb, never copied to a clone. */
	struct sk_buff *ts_next;
	struct sk_buff *ts_prev;
	volatile uint32_t refcount; // header references (skb_get/skb_put)
	uint32_t pool_sig; // header magic, catches stray pointers

	net_device_t *dev; // ingress / egress device
	uint8_t *head; // start of the data area
	uint8_t *data; // start of valid data
	uint8_t *tail; // end of valid data (data + len)
	uint8_t *end; // end of the data area

	uint32_t len; // bytes of valid data
	uint32_t truesize; // memory charged for this skb (header + area)
	uint16_t protocol; // ETH_P_* of the payload (host order)
	uint16_t mac_header; // offsets from head, SKB_HDR_UNSET if unknown
	uint16_t network_header;
	uint16_t transport_header;

	uint8_t ip_summed; // CHECKSUM_*
	uint8_t pkt_type; // PACKET_*
	uint8_t cloned; // the data is (or was) shared with another header
	uint8_t hdr_dynamic; // header from slab, not the static pools
	uint16_t csum_start; // CHECKSUM_PARTIAL: offset from head
	uint16_t csum_offset; // ... and of the checksum field from there
	uint32_t csum; // CHECKSUM_COMPLETE sum
	uint32_t hash; // flow hash (0: not computed)

	uint64_t tstamp_ns; // receive / send time (monotonic ns), 0 = unset

	// Legacy layer-3 metadata (receive side), filled as the skb travels.
	uint32_t src_ip; // host order
	uint32_t dst_ip; // host order
	uint8_t ip_proto;
	uint8_t rx_ttl;
	uint16_t _pad16;
	uint32_t _pad32;

	skb_shared_info_t *shinfo; // the data area's shared state

	/* Called once when the header is released (before the data is), with
	 * the skb still intact: the hook for socket memory accounting.  May
	 * run in any context the skb is freed in (also hard IRQ). */
	void (*destructor)(struct sk_buff *skb);
	void *sk; // owner for the destructor

	/* Private scratch of the current owner (8-aligned). */
	uint8_t cb[SKB_CB_SIZE] __attribute__((aligned(8)));
} sk_buff_t;

/* One data allocation: the embedded header, the shared info and the area. */
typedef struct skb_slot {
	sk_buff_t skb;
	skb_shared_info_t sh;
	struct skb_slot *free_next; // free-list linkage
	uint64_t _align;
	uint8_t data[];
} skb_slot_t;

#define SKB_HDR_MAGIC 0x534B4248u // 'SKBH', live header
#define SKB_HDR_DEAD 0x534B4244u // 'SKBD', released header

// ---- lifecycle ----

void skb_pool_init(void);
uint64_t skb_get_alloc_failures(void);

// Allocate an skb whose data area holds at least `min_payload` bytes after
// SKB_HEADROOM bytes of headroom.  Returns NULL when no memory is left (the
// static pool, the reserve and -- with interrupts enabled -- the slab are
// exhausted, or the dynamic limit is reached) or the request exceeds
// SKB_MAX_ALLOC.  The skb has one reference, len 0, no dev.  Any context.
sk_buff_t *skb_alloc(uint32_t min_payload);

// The same with the headroom chosen by the caller.
sk_buff_t *skb_alloc_headroom(uint32_t headroom, uint32_t min_payload);

// An skb in the smallest class that holds `headroom' + `min_payload'
// bytes: small, one of the fitted classes, or jumbo.  A fitted slot comes
// from its class's reserve or -- interrupts enabled only -- the slab; NULL
// when there is none (the caller keeps what it has: this never takes a
// larger class instead).  Any context.
sk_buff_t *skb_alloc_fit(uint32_t headroom, uint32_t min_payload);

// A private copy of `skb' (headroom, data, header fields) in a buffer of
// the class skb_alloc_fit() picks for it -- only when that buffer costs
// less memory than the skb's own; NULL otherwise (or without memory).
sk_buff_t *skb_copy_fit(const sk_buff_t *skb);

// Take an extra reference on this header.
static inline sk_buff_t *skb_get(sk_buff_t *skb)
{
	if (skb)
		__atomic_fetch_add(&skb->refcount, 1, __ATOMIC_ACQ_REL);
	return skb;
}

// Drop a reference.  At zero the destructor runs, the header is released
// and the data area goes back to its pool once no other header uses it.
// NULL is ignored.  Any context.
void skb_put(sk_buff_t *skb);

static inline void skb_free(sk_buff_t *skb)
{
	skb_put(skb);
}

// A new header sharing this skb's data (refcount 1, cloned).  NULL when no
// header is available.  The data must be treated as read-only by both.
sk_buff_t *skb_clone(sk_buff_t *skb);

// A private copy (header and data, same headroom).  NULL on no memory.
sk_buff_t *skb_copy(const sk_buff_t *skb);

// A private copy with at least `headroom` in front and `tailroom` behind.
sk_buff_t *skb_copy_expand(const sk_buff_t *skb, uint32_t headroom,
			   uint32_t tailroom);

// Make the skb private: returns it unchanged when its data is not shared,
// otherwise a copy (and the original skb is released).  NULL
// when the copy failed -- the original's reference is dropped then too.
sk_buff_t *skb_unshare(sk_buff_t *skb);

// Ensure `headroom` bytes of PRIVATE headroom: returns the skb itself when
// it already has them and its data is not shared, otherwise a copy with
// the room (the original reference is dropped).  NULL on failure (original
// dropped as well).
sk_buff_t *skb_realloc_headroom(sk_buff_t *skb, uint32_t headroom);

static inline int skb_cloned(const sk_buff_t *skb)
{
	return skb->cloned &&
	       __atomic_load_n(&skb->shinfo->dataref, __ATOMIC_ACQUIRE) > 1;
}

static inline int skb_shared(const sk_buff_t *skb)
{
	return __atomic_load_n(&skb->refcount, __ATOMIC_ACQUIRE) > 1;
}

// Before writing headers into the headroom: ensure `headroom' bytes of it
// that no other header shares (skb_realloc_headroom's rule: the skb itself
// when it has the room and its data is not shared with a clone, otherwise
// a private copy with it; the original reference is dropped, its
// destructor runs then as for any copy).  NULL on failure.
static inline sk_buff_t *skb_cow_head(sk_buff_t *skb, uint32_t headroom)
{
	return skb_realloc_headroom(skb, headroom);
}

// ---- pointer manipulation (bounds are checked; violations BUG) ----

static inline uint32_t skb_headroom(const sk_buff_t *skb)
{
	return (uint32_t)(skb->data - skb->head);
}

static inline uint32_t skb_tailroom(const sk_buff_t *skb)
{
	return (uint32_t)(skb->end - skb->tail);
}

// Reserve headroom by advancing data and tail.  Only on an empty skb.
static inline void skb_reserve(sk_buff_t *skb, uint32_t n)
{
	BUG_ON(skb->len != 0 || n > skb_tailroom(skb));
	skb->data += n;
	skb->tail += n;
}

// Append n bytes at the tail; returns a pointer to the new region.
static inline uint8_t *skb_append(sk_buff_t *skb, uint32_t n)
{
	uint8_t *p = skb->tail;

	BUG_ON(n > skb_tailroom(skb));
	skb->tail += n;
	skb->len += n;
	return p;
}

// Prepend n bytes (move data back into the headroom); returns the new data.
static inline uint8_t *skb_push(sk_buff_t *skb, uint32_t n)
{
	BUG_ON(n > skb_headroom(skb));
	skb->data -= n;
	skb->len += n;
	return skb->data;
}

// Older name of skb_push().
static inline uint8_t *skb_prepend(sk_buff_t *skb, uint32_t n)
{
	return skb_push(skb, n);
}

// Remove n bytes from the front; returns the new data pointer, or NULL
// (and changes nothing) when the skb is shorter than n.
static inline uint8_t *skb_pull(sk_buff_t *skb, uint32_t n)
{
	if (n > skb->len)
		return NULL;
	skb->data += n;
	skb->len -= n;
	return skb->data;
}

// Cut the skb to `len` bytes (no-op when it is not longer).
static inline void skb_trim(sk_buff_t *skb, uint32_t len)
{
	if (skb->len > len) {
		skb->len = len;
		skb->tail = skb->data + len;
	}
}

// Header offsets (relative to head, so they survive push/pull).
static inline void skb_reset_mac_header(sk_buff_t *skb)
{
	skb->mac_header = (uint16_t)(skb->data - skb->head);
}

static inline void skb_reset_network_header(sk_buff_t *skb)
{
	skb->network_header = (uint16_t)(skb->data - skb->head);
}

static inline void skb_reset_transport_header(sk_buff_t *skb)
{
	skb->transport_header = (uint16_t)(skb->data - skb->head);
}

static inline void skb_set_network_header(sk_buff_t *skb, int off)
{
	skb->network_header = (uint16_t)(skb->data - skb->head + off);
}

static inline void skb_set_transport_header(sk_buff_t *skb, int off)
{
	skb->transport_header = (uint16_t)(skb->data - skb->head + off);
}

static inline uint8_t *skb_mac_header(const sk_buff_t *skb)
{
	return skb->head + skb->mac_header;
}

static inline uint8_t *skb_network_header(const sk_buff_t *skb)
{
	return skb->head + skb->network_header;
}

static inline uint8_t *skb_transport_header(const sk_buff_t *skb)
{
	return skb->head + skb->transport_header;
}

static inline int skb_network_offset(const sk_buff_t *skb)
{
	return (int)(skb_network_header(skb) - skb->data);
}

// Copy `len` bytes from offset `off` of the valid data into `to`.  Returns
// 0, or -1 when the range is outside the data.
int skb_copy_bits(const sk_buff_t *skb, uint32_t off, void *to, uint32_t len);

// ---- checksums (ones' complement, RFC 1071) ----

// Running 32-bit sum of `len` bytes added to `sum` (not folded).
uint32_t csum_partial(const void *buf, uint32_t len, uint32_t sum);

static inline uint16_t csum_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)~sum;
}

// The IPv4 pseudo header (addresses and protocol in host order, len in
// host order) added to `sum`, not folded.  Byte order of the result is the
// one of the summed packet bytes (network order on little-endian loads).
uint32_t csum_tcpudp_nofold(uint32_t saddr, uint32_t daddr, uint16_t len,
			    uint8_t proto, uint32_t sum);

// Checksum over the skb's data from `off` for `len` bytes, added to `sum`.
uint32_t skb_checksum(const sk_buff_t *skb, uint32_t off, uint32_t len,
		      uint32_t sum);

// ---- statistics and memory accounting ----

typedef struct skb_mem_stats {
	uint64_t alloc[SKB_NR_CLS]; // successful allocations per class
	uint64_t alloc_dynamic; // ... of which from dynamic memory
	uint64_t promote; // small requests served from the jumbo class
	uint64_t fail; // allocation failures
	uint64_t clone; // clones made
	uint64_t clone_fail; // no header for a clone
	uint64_t copy; // skb_copy / unshare copies
	uint32_t static_free[SKB_NR_CLS]; // free static slots
	uint32_t static_total[SKB_NR_CLS];
	uint32_t reserve[SKB_NR_CLS]; // dynamic slots on the reserve
	uint32_t dyn_slots[SKB_NR_CLS]; // dynamic slots in existence
	uint64_t dyn_bytes; // dynamic memory in existence (slots)
	uint64_t dyn_limit; // cap of dyn_bytes
	uint64_t inuse_bytes; // truesize of every live skb
	uint64_t refill_runs; // housekeeping refills that allocated
	uint64_t trim_frees; // dynamic slots given back to the slab
} skb_mem_stats_t;

void skb_get_mem_stats(skb_mem_stats_t *out);

// Refill the dynamic reserve and trim excess dynamic memory.  Process
// context with interrupts enabled and no spinlock held (the network
// housekeeping softirq).  Returns 1 when it would like to run again soon.
int skb_reserve_maintain(void);

// 1 when the reserve is below its low-water mark (a refill is due).
int skb_reserve_low(void);

// ---- queues ----

/* A doubly linked FIFO of skbs with its own lock.  The locked calls take
 * the lock with interrupts disabled and are usable from any context; the
 * __ variants expect the caller to hold q->lock (or to own the queue
 * exclusively).  A queued skb belongs to the queue: its next/prev are the
 * queue's. */
typedef struct skb_queue {
	sk_buff_t *head;
	sk_buff_t *tail;
	uint32_t len; // skbs on the queue
	uint32_t _pad;
	spinlock_t lock;
} skb_queue_t;

void skb_queue_init(skb_queue_t *q, const char *name);
// Same, under the name other stacks use.
static inline void skb_queue_head_init(skb_queue_t *q)
{
	skb_queue_init(q, "skb_queue");
}

static inline uint32_t skb_queue_len(const skb_queue_t *q)
{
	return __atomic_load_n(&q->len, __ATOMIC_ACQUIRE);
}

static inline int skb_queue_empty(const skb_queue_t *q)
{
	return skb_queue_len(q) == 0;
}

void __skb_queue_tail(skb_queue_t *q, sk_buff_t *skb);
void __skb_queue_head(skb_queue_t *q, sk_buff_t *skb);
sk_buff_t *__skb_dequeue(skb_queue_t *q);
sk_buff_t *__skb_dequeue_tail(skb_queue_t *q);
void __skb_unlink(sk_buff_t *skb, skb_queue_t *q);

static inline sk_buff_t *skb_peek(const skb_queue_t *q)
{
	return q->head;
}

static inline sk_buff_t *skb_peek_tail(const skb_queue_t *q)
{
	return q->tail;
}

void skb_queue_tail(skb_queue_t *q, sk_buff_t *skb); // enqueue at the tail
void skb_queue_head(skb_queue_t *q, sk_buff_t *skb); // enqueue at the head
sk_buff_t *skb_dequeue(skb_queue_t *q); // pop the head, NULL if empty
sk_buff_t *skb_dequeue_tail(skb_queue_t *q); // pop the tail
void skb_unlink(sk_buff_t *skb, skb_queue_t *q);
// Move every skb of `from` to the tail of `to` (from becomes empty).  Both
// locks are taken, `from` first: callers must use one consistent order.
void skb_queue_splice_tail_init(skb_queue_t *from, skb_queue_t *to);
// Move the whole queue onto a private list head (returned, NULL-terminated
// through ->next), leaving it empty.  For draining outside the lock.
sk_buff_t *skb_queue_take_all(skb_queue_t *q, uint32_t *count);
// Free every skb on the queue (outside the lock).
void skb_queue_purge(skb_queue_t *q);

#endif // LIKEOS_SKB_H
