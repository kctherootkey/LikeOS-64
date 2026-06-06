// LikeOS-64 — Socket Buffer (sk_buff)
//
// A reference-counted, ownership-transferring packet buffer used end-to-end
// by the network stack.  Replaces the previous "static byte buffer + global
// TX spinlock" pattern at every protocol layer.
//
// Why
// ---
// The old design held a per-layer spinlock across the call to the layer
// below.  The bottom layer (ipv4_send_common) blocks with `sti;hlt` while
// waiting for an ARP reply.  Two failure modes follow:
//
//   1. Same-CPU re-entry: when interrupts are re-enabled, an inbound packet
//      IRQ runs the receive path on the same CPU and re-enters the lock the
//      caller is already holding -> deadlock.
//
//   2. Cross-CPU TLB-shootdown timeout: another CPU spins on a contended TX
//      lock with interrupts disabled (spin_lock_irqsave) for milliseconds.
//      It cannot acknowledge a TLB-shootdown IPI in time, and the originator
//      reports `SMP: TLB shootdown sync timeout`.
//
// Design
// ------
// A `sk_buff` owns its data buffer.  Buffers come from one of two
// size-classed pools (small 1536 B / jumbo 65535 B) so that no allocator
// call from a hard IRQ ever needs to touch the page allocator.  Free-list
// locks are held for ~10 cycles only, so they never delay TLB IPIs.
//
// Ownership rules:
//   * `skb_alloc` returns a new skb with refcount = 1 (the caller owns it).
//   * Passing an skb into a queueing function (e.g. `skb_queue_tail`,
//     receive enqueue, ARP pending list) transfers ownership; the caller
//     must not touch the skb afterwards.
//   * Passing an skb into a synchronous send function similarly transfers
//     ownership; the callee must `skb_put` (decrement) when done.
//   * Use `skb_get` to take a second reference if the original owner needs
//     to keep accessing the skb after the handoff.
//   * `skb_put` decrements; the buffer is returned to its pool at refcount 0.
//
// No spinlock is ever held across a layer-to-layer call.  All shared state
// is protected by short-held, per-resource locks (per-queue, per-socket,
// per-pool freelist).

#ifndef LIKEOS_SKB_H
#define LIKEOS_SKB_H

#include "types.h"
#include "sched.h"   // spinlock_t

struct net_device;
typedef struct net_device net_device_t;

#define SKB_SMALL_DATA   1536      // >= 1518 (max Ethernet frame), 16-aligned
#define SKB_JUMBO_DATA   65535     // max IPv4 datagram
#define SKB_HEADROOM     128       // reserved at front for header prepends

typedef struct sk_buff {
    struct sk_buff* next;          // queue linkage (caller-managed)
    volatile uint32_t refcount;    // managed by skb_get/skb_put
    uint32_t pool_sig;             // identifies which pool to return to

    net_device_t* dev;             // ingress / egress device
    uint8_t*  head;                // start of underlying buffer
    uint8_t*  data;                // start of valid data
    uint8_t*  tail;                // end of valid data (data + len)
    uint8_t*  end;                 // end of underlying buffer

    uint16_t  len;                 // length of valid data
    uint16_t  protocol;            // ETHERTYPE_* (set by eth_rx)

    // Layer-3 metadata filled in as the skb travels up the stack:
    uint32_t  src_ip;              // host order, set by ipv4_rx
    uint32_t  dst_ip;              // host order, set by ipv4_rx
    uint8_t   ip_proto;            // IP_PROTO_* (set by ipv4_rx)
    uint8_t   rx_ttl;              // received TTL (for traceroute responses)
    uint16_t  _pad;

    uint64_t  timestamp_us;        // for latency measurement (icmp echo)

    // 8 bytes of scratch for protocol-specific context (e.g. ARP-pending
    // info, TCP per-segment flags) -- avoid storing pointers that might
    // outlive the skb.
    uint64_t  cb;
} sk_buff_t;

// ---- pool-managed lifecycle ----

void       skb_pool_init(void);

// Allocate an skb whose data area is at least `min_payload` bytes long.
// Returns NULL only on pool exhaustion of *both* size classes (effectively
// impossible at the configured pool sizes).  The returned skb has:
//   refcount = 1, len = 0, headroom = SKB_HEADROOM, no dev set.
sk_buff_t* skb_alloc(uint32_t min_payload);

// Take an extra reference (e.g. when queueing a copy elsewhere).
static inline sk_buff_t* skb_get(sk_buff_t* skb) {
    if (skb) __atomic_fetch_add(&skb->refcount, 1, __ATOMIC_ACQ_REL);
    return skb;
}

// Drop a reference; returns the buffer to the pool at refcount 0.
void skb_put(sk_buff_t* skb);

// ---- buffer pointer manipulation (caller's responsibility to stay in
//      bounds; debug builds may add asserts) ----

// Reserve headroom by advancing data.  Use immediately after alloc when you
// know the upper-layer header sizes.
static inline void skb_reserve(sk_buff_t* skb, uint32_t n) {
    skb->data += n;
    skb->tail += n;
}

// Append n bytes; returns pointer to the newly added region.
static inline uint8_t* skb_append(sk_buff_t* skb, uint32_t n) {
    uint8_t* p = skb->tail;
    skb->tail += n;
    skb->len  += n;
    return p;
}

// Prepend n bytes (move data pointer back into headroom); returns new data ptr.
static inline uint8_t* skb_prepend(sk_buff_t* skb, uint32_t n) {
    skb->data -= n;
    skb->len  += n;
    return skb->data;
}

// Remove n bytes from the front of the data (advance data, shrink len).
static inline void skb_pull(sk_buff_t* skb, uint32_t n) {
    skb->data += n;
    skb->len  -= n;
}

// ---- minimal queue helper (singly-linked, caller provides lock) ----

typedef struct skb_queue {
    sk_buff_t* head;
    sk_buff_t* tail;
    uint32_t   len;
    spinlock_t lock;
} skb_queue_t;

void       skb_queue_init(skb_queue_t* q, const char* name);
void       skb_queue_tail(skb_queue_t* q, sk_buff_t* skb);
sk_buff_t* skb_queue_head(skb_queue_t* q);   // pop head, NULL if empty

#endif // LIKEOS_SKB_H
