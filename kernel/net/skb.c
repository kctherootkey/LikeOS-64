// LikeOS-64 — sk_buff pool and queue implementation
//
// Two-class pool (small 1536 B data / jumbo 65535 B data).  Each pool slot
// holds one sk_buff_t header followed by its data buffer in a single static
// allocation, so we never touch the page or slab allocator on the packet
// fast path.  Free-list manipulation is protected by a per-class spinlock
// held only long enough to swing one `next` pointer.

#include "../../include/kernel/skb.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"

#define SKB_SMALL_COUNT   512
#define SKB_JUMBO_COUNT     8

#define SKB_SIG_SMALL  0x534B5342u   // 'SKSB'
#define SKB_SIG_JUMBO  0x4A4B5342u   // 'JKSB'

// Each pool slot is one sk_buff_t followed immediately by its data area.
typedef struct skb_slot_small {
    sk_buff_t skb;
    uint8_t   data[SKB_SMALL_DATA];
} skb_slot_small_t;

typedef struct skb_slot_jumbo {
    sk_buff_t skb;
    uint8_t   data[SKB_JUMBO_DATA];
} skb_slot_jumbo_t;

static skb_slot_small_t small_pool[SKB_SMALL_COUNT];
static skb_slot_jumbo_t jumbo_pool[SKB_JUMBO_COUNT];

static sk_buff_t* small_freelist;
static sk_buff_t* jumbo_freelist;
static spinlock_t small_lock = SPINLOCK_INIT("skb_small");
static spinlock_t jumbo_lock = SPINLOCK_INIT("skb_jumbo");
static volatile int skb_pool_ready = 0;

// Diagnostics (incremented under lock, so monotonic but not atomic-read-safe).
static volatile uint64_t skb_alloc_small_ok;
static volatile uint64_t skb_alloc_jumbo_ok;
static volatile uint64_t skb_alloc_smallfail_promote;
static volatile uint64_t skb_alloc_jumbofail_demote;
static volatile uint64_t skb_alloc_total_fail;

static inline void skb_init_slot(sk_buff_t* skb, uint8_t* data, uint32_t end_off,
                                 uint32_t sig) {
    skb->next     = NULL;
    skb->refcount = 0;
    skb->pool_sig = sig;
    skb->dev      = NULL;
    skb->head     = data;
    skb->data     = data;
    skb->tail     = data;
    skb->end      = data + end_off;
    skb->len      = 0;
    skb->protocol = 0;
    skb->src_ip   = 0;
    skb->dst_ip   = 0;
    skb->ip_proto = 0;
    skb->rx_ttl   = 0;
    skb->_pad     = 0;
    skb->timestamp_us = 0;
    skb->cb       = 0;
}

void skb_pool_init(void) {
    small_freelist = NULL;
    for (int i = SKB_SMALL_COUNT - 1; i >= 0; i--) {
        sk_buff_t* skb = &small_pool[i].skb;
        skb_init_slot(skb, small_pool[i].data, SKB_SMALL_DATA, SKB_SIG_SMALL);
        skb->next = small_freelist;
        small_freelist = skb;
    }
    jumbo_freelist = NULL;
    for (int i = SKB_JUMBO_COUNT - 1; i >= 0; i--) {
        sk_buff_t* skb = &jumbo_pool[i].skb;
        skb_init_slot(skb, jumbo_pool[i].data, SKB_JUMBO_DATA, SKB_SIG_JUMBO);
        skb->next = jumbo_freelist;
        jumbo_freelist = skb;
    }
    skb_pool_ready = 1;
    kprintf("skb: pool ready (%u small, %u jumbo)\n",
            (unsigned)SKB_SMALL_COUNT, (unsigned)SKB_JUMBO_COUNT);
}

static sk_buff_t* skb_pop_small(void) {
    uint64_t f;
    spin_lock_irqsave(&small_lock, &f);
    sk_buff_t* skb = small_freelist;
    if (skb) small_freelist = skb->next;
    spin_unlock_irqrestore(&small_lock, f);
    return skb;
}

static sk_buff_t* skb_pop_jumbo(void) {
    uint64_t f;
    spin_lock_irqsave(&jumbo_lock, &f);
    sk_buff_t* skb = jumbo_freelist;
    if (skb) jumbo_freelist = skb->next;
    spin_unlock_irqrestore(&jumbo_lock, f);
    return skb;
}

static void skb_push_small(sk_buff_t* skb) {
    uint64_t f;
    spin_lock_irqsave(&small_lock, &f);
    skb->next = small_freelist;
    small_freelist = skb;
    spin_unlock_irqrestore(&small_lock, f);
}

static void skb_push_jumbo(sk_buff_t* skb) {
    uint64_t f;
    spin_lock_irqsave(&jumbo_lock, &f);
    skb->next = jumbo_freelist;
    jumbo_freelist = skb;
    spin_unlock_irqrestore(&jumbo_lock, f);
}

static void skb_reset(sk_buff_t* skb) {
    skb->next     = NULL;
    skb->refcount = 1;
    skb->dev      = NULL;
    skb->data     = skb->head + SKB_HEADROOM;
    skb->tail     = skb->data;
    skb->len      = 0;
    skb->protocol = 0;
    skb->src_ip   = 0;
    skb->dst_ip   = 0;
    skb->ip_proto = 0;
    skb->rx_ttl   = 0;
    skb->timestamp_us = 0;
    skb->cb       = 0;
}

sk_buff_t* skb_alloc(uint32_t min_payload) {
    if (!skb_pool_ready) return NULL;

    // Account for headroom: a "small" skb actually has SKB_SMALL_DATA bytes
    // of buffer total, of which SKB_HEADROOM are reserved.
    uint32_t need = min_payload + SKB_HEADROOM;
    int want_jumbo = (need > SKB_SMALL_DATA);

    if (need > SKB_JUMBO_DATA) {
        skb_alloc_total_fail++;
        return NULL;
    }

    sk_buff_t* skb;
    if (want_jumbo) {
        skb = skb_pop_jumbo();
        if (!skb) {
            // Jumbo exhausted but caller asked for jumbo -- cannot demote.
            skb_alloc_total_fail++;
            return NULL;
        }
        skb_alloc_jumbo_ok++;
    } else {
        skb = skb_pop_small();
        if (!skb) {
            // Small exhausted: promote to jumbo if available (rare).
            skb = skb_pop_jumbo();
            if (!skb) {
                skb_alloc_total_fail++;
                return NULL;
            }
            skb_alloc_smallfail_promote++;
        } else {
            skb_alloc_small_ok++;
        }
    }

    skb_reset(skb);
    return skb;
}

void skb_put(sk_buff_t* skb) {
    if (!skb) return;
    uint32_t old = __atomic_fetch_sub(&skb->refcount, 1, __ATOMIC_ACQ_REL);
    if (old != 1) return;          // still referenced by someone else
    // refcount has dropped to 0 -- return to pool.
    if (skb->pool_sig == SKB_SIG_SMALL) {
        skb_push_small(skb);
    } else if (skb->pool_sig == SKB_SIG_JUMBO) {
        skb_push_jumbo(skb);
    }
    // Bad signature: silently leak (better than corrupting freelist).
}

// ---- skb_queue ----

void skb_queue_init(skb_queue_t* q, const char* name) {
    q->head = NULL;
    q->tail = NULL;
    q->len  = 0;
    spinlock_init(&q->lock, name);
}

void skb_queue_tail(skb_queue_t* q, sk_buff_t* skb) {
    if (!skb) return;
    skb->next = NULL;
    uint64_t f;
    spin_lock_irqsave(&q->lock, &f);
    if (q->tail) q->tail->next = skb;
    else         q->head       = skb;
    q->tail = skb;
    q->len++;
    spin_unlock_irqrestore(&q->lock, f);
}

sk_buff_t* skb_queue_head(skb_queue_t* q) {
    uint64_t f;
    spin_lock_irqsave(&q->lock, &f);
    sk_buff_t* skb = q->head;
    if (skb) {
        q->head = skb->next;
        if (!q->head) q->tail = NULL;
        q->len--;
        skb->next = NULL;
    }
    spin_unlock_irqrestore(&q->lock, f);
    return skb;
}
