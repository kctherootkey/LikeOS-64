// LikeOS — sk_buff allocation, sharing, queues and checksums
//
// See kernel/net/skb.h for the layout, the memory model and the ownership
// rules.
//
// Pools
// -----
// Per general size class (small, jumbo):
//   static free list   slots carved out of a static array at boot; the
//                      fast path, usable in any context;
//   reserve            dynamic (slab) slots that are allocated and free,
//                      ready to be handed out in any context;
//   dynamic in use     dynamic slots handed out.
// A released dynamic slot goes back onto the reserve, never to the slab
// allocator directly; skb_reserve_maintain() (housekeeping softirq) tops
// the reserve up while the static pool is under pressure and gives memory
// back once the pressure is gone.  Clone headers have the same two-level
// scheme (a static header pool plus slab headers parked on a free list).
// The fitted classes (skb_alloc_fit) are dynamic only: a slot comes from
// the reserve of released ones or from the slab, and the reserve is given
// back once the class has gone unused for SKB_RELAX_MS.
//
// Locks: one per class (static free list and reserve), one for the clone
// headers.  All are leaves, taken with interrupts disabled, held for a few
// pointer swings.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/skb.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/mm/slab.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>

/* Static pool sizing: every packet in flight through the stack -- NIC RX
 * waiting in the backlog, a transmit queue, an ARP-pending queue -- holds
 * one slot.  A single sock_send() can emit min(cwnd, TCP_MAX_INFLIGHT)
 * segments in one burst, so a handful of concurrent connections need well
 * over 512 small slots.  2048 small (~3.6 MB) + 128 jumbo (~8.4 MB) is the
 * static part; the dynamic part grows beyond it on demand. */
#define SKB_SMALL_COUNT 2048
#define SKB_JUMBO_COUNT 128
#define SKB_CLONE_HDRS 1024

/* Dynamic memory: at most this much slab memory for packet buffers. */
#define SKB_DYN_LIMIT (64ULL << 20)

/* Reserve policy per class: a refill is due when the static free list has
 * fewer than `low' slots; it tops the reserve up to `target'; slots beyond
 * `high' on the reserve are given back at once, and the whole reserve once
 * the static pool has been above `relax' for SKB_RELAX_TICKS_MS.  The
 * fitted classes are never topped up (low and target 0); `high' keeps at
 * most about half a megabyte of released slots per class. */
static const uint32_t res_low[SKB_NR_CLS] = { 256, 16, 0, 0, 0, 0, 0, 0, 0 };
static const uint32_t res_target[SKB_NR_CLS] = { 256, 8, 0, 0, 0, 0, 0, 0, 0 };
static const uint32_t res_high[SKB_NR_CLS] = { 1024, 32, 128, 64, 40,
					       32,   20, 16, 10 };
static const uint32_t res_relax[SKB_NR_CLS] = { 1024, 64, 0, 0, 0, 0, 0, 0, 0 };
#define SKB_RELAX_MS 10000

#define SLOT_BYTES(sz) ((sizeof(skb_slot_t) + (sz) + 63) & ~(size_t)63)
/* A fitted class's data area: what is left of `pages' pages after the
 * slab's large-allocation header and the slot header, so that the whole
 * slot (SLOT_BYTES, a multiple of 64 here) fills the pages without
 * spilling into one more. */
#define FIT_DATA(pages)                                                  \
	((uint32_t)((((pages) * PAGE_SIZE - sizeof(large_alloc_header_t)) & \
		     ~(size_t)63) -                                         \
		    sizeof(skb_slot_t)))

static const uint32_t cls_size[SKB_NR_CLS] = {
	SKB_SMALL_DATA, SKB_JUMBO_DATA, FIT_DATA(1), FIT_DATA(2),
	FIT_DATA(3),	FIT_DATA(4),	FIT_DATA(6), FIT_DATA(8),
	FIT_DATA(12),
};
static const uint32_t cls_count[SKB_NR_CLS] = { SKB_SMALL_COUNT,
						SKB_JUMBO_COUNT };
static const char *const cls_name[SKB_NR_CLS] = {
	"skb_small", "skb_jumbo", "skb_fit1",  "skb_fit2", "skb_fit3",
	"skb_fit4",  "skb_fit6",  "skb_fit8", "skb_fit12",
};
BUILD_BUG_ON(SKB_NR_CLS != 9);
BUILD_BUG_ON(FIT_DATA(1) <= SKB_SMALL_DATA);
BUILD_BUG_ON(FIT_DATA(12) >= SKB_JUMBO_DATA);

static uint8_t small_mem[SKB_SMALL_COUNT * SLOT_BYTES(SKB_SMALL_DATA)]
	__attribute__((aligned(64)));
static uint8_t jumbo_mem[SKB_JUMBO_COUNT * SLOT_BYTES(SKB_JUMBO_DATA)]
	__attribute__((aligned(64)));
static sk_buff_t clone_hdrs[SKB_CLONE_HDRS];

struct skb_class {
	spinlock_t lock;
	skb_slot_t *static_free; // static slots
	skb_slot_t *reserve; // free dynamic slots
	uint32_t nstatic_free;
	uint32_t nreserve;
	uint32_t ndyn; // dynamic slots in existence (reserve + in use)
	uint32_t _pad;
	uint8_t *mem_lo, *mem_hi; // the static array
};

static struct skb_class classes[SKB_NR_CLS];

static spinlock_t hdr_lock = SPINLOCK_INIT("skb_hdr");
static sk_buff_t *hdr_free; // free clone headers (static and dynamic)
static uint32_t hdr_nfree;
static uint32_t hdr_ndyn; // dynamic clone headers in existence
static volatile int skb_pool_ready;

static volatile uint64_t st_alloc[SKB_NR_CLS];
static volatile uint64_t st_alloc_dyn;
static volatile uint64_t st_promote;
static volatile uint64_t st_fail;
static volatile uint64_t st_clone;
static volatile uint64_t st_clone_fail;
static volatile uint64_t st_copy;
static volatile uint64_t st_dyn_bytes;
static volatile uint64_t st_inuse_bytes;
static volatile uint64_t st_refill_runs;
static volatile uint64_t st_trim_frees;
static volatile uint64_t relax_since[SKB_NR_CLS]; // tick pressure ended
static volatile uint64_t fit_last_use[SKB_NR_CLS]; // tick a fitted slot went out
static volatile int refill_kicked;

void net_housekeeping_kick(void); // net.c

static inline void stat_add(volatile uint64_t *c, uint64_t v)
{
	__atomic_fetch_add(c, v, __ATOMIC_RELAXED);
}

static inline void stat_sub(volatile uint64_t *c, uint64_t v)
{
	__atomic_fetch_sub(c, v, __ATOMIC_RELAXED);
}

uint64_t skb_get_alloc_failures(void)
{
	return __atomic_load_n(&st_fail, __ATOMIC_RELAXED);
}

static size_t slot_bytes(int cls)
{
	return SLOT_BYTES(cls_size[cls]);
}

void skb_pool_init(void)
{
	for (int c = 0; c < SKB_NR_CLS; c++) {
		struct skb_class *k = &classes[c];
		uint8_t *mem = c == SKB_CLS_SMALL ? small_mem :
			       c == SKB_CLS_JUMBO ? jumbo_mem :
						    NULL; // fitted: no static pool
		size_t sb = slot_bytes(c);

		spinlock_init(&k->lock, cls_name[c]);
		k->static_free = NULL;
		k->reserve = NULL;
		k->nstatic_free = 0;
		k->nreserve = 0;
		k->ndyn = 0;
		k->mem_lo = mem;
		k->mem_hi = mem ? mem + sb * cls_count[c] : NULL;
		for (int i = (int)cls_count[c] - 1; i >= 0; i--) {
			skb_slot_t *s = (skb_slot_t *)(mem + sb * (size_t)i);

			s->skb.pool_sig = SKB_HDR_DEAD;
			s->sh.cls = (uint8_t)c;
			s->sh.dynamic = 0;
			s->sh.size = cls_size[c];
			s->sh.slot = s;
			s->free_next = k->static_free;
			k->static_free = s;
			k->nstatic_free++;
		}
	}
	hdr_free = NULL;
	hdr_nfree = 0;
	for (int i = SKB_CLONE_HDRS - 1; i >= 0; i--) {
		clone_hdrs[i].pool_sig = SKB_HDR_DEAD;
		clone_hdrs[i].hdr_dynamic = 0;
		clone_hdrs[i].next = hdr_free;
		hdr_free = &clone_hdrs[i];
		hdr_nfree++;
	}
	skb_pool_ready = 1;
	kprintf("skb: pool ready (%u small, %u jumbo, %u clone headers)\n",
		(unsigned)SKB_SMALL_COUNT, (unsigned)SKB_JUMBO_COUNT,
		(unsigned)SKB_CLONE_HDRS);
}

// ---- slots ----

static skb_slot_t *slot_take(int cls, int *dynamic)
{
	struct skb_class *k = &classes[cls];
	skb_slot_t *s;
	uint64_t f;

	spin_lock_irqsave(&k->lock, &f);
	s = k->static_free;
	if (s) {
		k->static_free = s->free_next;
		k->nstatic_free--;
		*dynamic = 0;
	} else if ((s = k->reserve) != NULL) {
		k->reserve = s->free_next;
		k->nreserve--;
		*dynamic = 1;
	}
	spin_unlock_irqrestore(&k->lock, f);
	return s;
}

/* A new dynamic slot from the slab.  Only with interrupts enabled. */
static skb_slot_t *slot_new_dynamic(int cls)
{
	size_t sb = slot_bytes(cls);
	struct skb_class *k = &classes[cls];
	skb_slot_t *s;
	uint64_t f;

	if (irqs_disabled())
		return NULL;
	if (__atomic_load_n(&st_dyn_bytes, __ATOMIC_RELAXED) + sb >
	    SKB_DYN_LIMIT)
		return NULL;
	s = slab_alloc(sb);
	if (!s)
		return NULL;
	stat_add(&st_dyn_bytes, sb);
	s->skb.pool_sig = SKB_HDR_DEAD;
	s->sh.cls = (uint8_t)cls;
	s->sh.dynamic = 1;
	s->sh.size = cls_size[cls];
	s->sh.slot = s;
	s->free_next = NULL;
	spin_lock_irqsave(&k->lock, &f);
	k->ndyn++;
	spin_unlock_irqrestore(&k->lock, f);
	return s;
}

static void slot_release(skb_slot_t *s)
{
	int cls = s->sh.cls;
	struct skb_class *k;
	uint64_t f;

	BUG_ON(cls >= SKB_NR_CLS);
	k = &classes[cls];
	stat_sub(&st_inuse_bytes, slot_bytes(cls));
	s->skb.pool_sig = SKB_HDR_DEAD;
	spin_lock_irqsave(&k->lock, &f);
	if (s->sh.dynamic) {
		s->free_next = k->reserve;
		k->reserve = s;
		k->nreserve++;
	} else {
		WARN_ON((uint8_t *)s < k->mem_lo || (uint8_t *)s >= k->mem_hi);
		s->free_next = k->static_free;
		k->static_free = s;
		k->nstatic_free++;
	}
	spin_unlock_irqrestore(&k->lock, f);
}

int skb_reserve_low(void)
{
	for (int c = 0; c < SKB_NR_CLS; c++) {
		struct skb_class *k = &classes[c];

		if (__atomic_load_n(&k->nstatic_free, __ATOMIC_RELAXED) <
			    res_low[c] &&
		    __atomic_load_n(&k->nreserve, __ATOMIC_RELAXED) <
			    res_target[c])
			return 1;
	}
	return 0;
}

static void maybe_kick_refill(int cls)
{
	struct skb_class *k = &classes[cls];

	if (__atomic_load_n(&k->nstatic_free, __ATOMIC_RELAXED) >= res_low[cls])
		return;
	if (__atomic_load_n(&k->nreserve, __ATOMIC_RELAXED) >= res_target[cls])
		return;
	if (__atomic_exchange_n(&refill_kicked, 1, __ATOMIC_ACQ_REL))
		return;
	net_housekeeping_kick();
}

static void skb_init_header(sk_buff_t *skb, skb_shared_info_t *sh,
			    uint8_t *head, uint32_t headroom)
{
	uint8_t dyn = skb->hdr_dynamic;

	mm_memset(skb, 0, sizeof(*skb));
	skb->hdr_dynamic = dyn;
	skb->refcount = 1;
	skb->pool_sig = SKB_HDR_MAGIC;
	skb->head = head;
	skb->data = head + headroom;
	skb->tail = skb->data;
	skb->end = head + sh->size;
	skb->shinfo = sh;
	skb->mac_header = SKB_HDR_UNSET;
	skb->network_header = SKB_HDR_UNSET;
	skb->transport_header = SKB_HDR_UNSET;
}

/* The skb of a slot just taken from class s->sh.cls. */
static sk_buff_t *slot_to_skb(skb_slot_t *s, int dyn, uint32_t headroom)
{
	if (dyn)
		stat_add(&st_alloc_dyn, 1);
	stat_add(&st_alloc[s->sh.cls], 1);
	stat_add(&st_inuse_bytes, slot_bytes(s->sh.cls));
	s->sh.dataref = 1;
	s->sh.gso_segs = 0;
	s->sh.gso_size = 0;
	s->skb.hdr_dynamic = 0;
	skb_init_header(&s->skb, &s->sh, s->data, headroom);
	s->skb.truesize = (uint32_t)slot_bytes(s->sh.cls);
	return &s->skb;
}

sk_buff_t *skb_alloc_headroom(uint32_t headroom, uint32_t min_payload)
{
	skb_slot_t *s = NULL;
	int cls, dyn = 0;

	if (!skb_pool_ready)
		return NULL;
	if (headroom > SKB_JUMBO_DATA || min_payload > SKB_JUMBO_DATA ||
	    headroom + min_payload > SKB_JUMBO_DATA) {
		WARN_RATELIMIT(1, "skb_alloc: oversized request %u+%u (max %u)",
			       headroom, min_payload,
			       (unsigned)SKB_JUMBO_DATA);
		stat_add(&st_fail, 1);
		return NULL;
	}
	cls = headroom + min_payload <= SKB_SMALL_DATA ? SKB_CLS_SMALL :
							 SKB_CLS_JUMBO;
	s = slot_take(cls, &dyn);
	if (!s) {
		s = slot_new_dynamic(cls);
		dyn = s != NULL;
	}
	if (!s && cls == SKB_CLS_SMALL) {
		s = slot_take(SKB_CLS_JUMBO, &dyn);
		if (s)
			stat_add(&st_promote, 1);
	}
	if (!s) {
		stat_add(&st_fail, 1);
		maybe_kick_refill(cls);
		return NULL;
	}
	maybe_kick_refill(s->sh.cls);
	return slot_to_skb(s, dyn, headroom);
}

sk_buff_t *skb_alloc(uint32_t min_payload)
{
	return skb_alloc_headroom(SKB_HEADROOM, min_payload);
}

sk_buff_t *skb_alloc_fit(uint32_t headroom, uint32_t min_payload)
{
	uint64_t need = (uint64_t)headroom + min_payload;
	skb_slot_t *s;
	int cls, dyn = 0;

	if (!skb_pool_ready)
		return NULL;
	if (need <= SKB_SMALL_DATA || need > cls_size[SKB_NR_CLS - 1])
		return skb_alloc_headroom(headroom, min_payload);
	for (cls = SKB_CLS_FIT0; cls < SKB_NR_CLS - 1; cls++)
		if (need <= cls_size[cls])
			break;
	s = slot_take(cls, &dyn);
	if (!s) {
		s = slot_new_dynamic(cls);
		dyn = s != NULL;
	}
	if (!s)
		return NULL;
	__atomic_store_n(&fit_last_use[cls], timer_ticks() | 1,
			 __ATOMIC_RELAXED);
	return slot_to_skb(s, dyn, headroom);
}

// ---- clone headers ----

static sk_buff_t *hdr_take(void)
{
	sk_buff_t *h;
	uint64_t f;

	spin_lock_irqsave(&hdr_lock, &f);
	h = hdr_free;
	if (h) {
		hdr_free = h->next;
		hdr_nfree--;
	}
	spin_unlock_irqrestore(&hdr_lock, f);
	if (h || irqs_disabled())
		return h;
	h = slab_alloc(sizeof(*h));
	if (!h)
		return NULL;
	mm_memset(h, 0, sizeof(*h));
	h->hdr_dynamic = 1;
	spin_lock_irqsave(&hdr_lock, &f);
	hdr_ndyn++;
	spin_unlock_irqrestore(&hdr_lock, f);
	return h;
}

static void hdr_release(sk_buff_t *h)
{
	uint64_t f;

	h->pool_sig = SKB_HDR_DEAD;
	spin_lock_irqsave(&hdr_lock, &f);
	h->next = hdr_free;
	hdr_free = h;
	hdr_nfree++;
	spin_unlock_irqrestore(&hdr_lock, f);
}

static inline int skb_hdr_embedded(const sk_buff_t *skb)
{
	return skb == &skb->shinfo->slot->skb;
}

void skb_put(sk_buff_t *skb)
{
	skb_shared_info_t *sh;
	uint32_t old;
	int embedded;

	if (!skb)
		return;
	if (unlikely(skb->pool_sig != SKB_HDR_MAGIC)) {
		WARN_ON_ONCE(1); /* skb_put of a dead or foreign header */
		return;
	}
	old = __atomic_fetch_sub(&skb->refcount, 1, __ATOMIC_ACQ_REL);
	if (unlikely(old == 0)) {
		WARN_ON_ONCE(1); /* refcount underflow (double free) */
		return;
	}
	if (old != 1)
		return;
	if (skb->destructor) {
		void (*d)(sk_buff_t *) = skb->destructor;

		skb->destructor = NULL;
		d(skb);
	}
	sh = skb->shinfo;
	embedded = skb_hdr_embedded(skb);
	skb->pool_sig = SKB_HDR_DEAD;
	if (!embedded) {
		stat_sub(&st_inuse_bytes, sizeof(sk_buff_t));
		hdr_release(skb);
	}
	/* The embedded header stays with its slot: it is reused only when
	 * the slot is, after the last header sharing the data is gone. */
	if (__atomic_sub_fetch(&sh->dataref, 1, __ATOMIC_ACQ_REL) == 0)
		slot_release(sh->slot);
}

sk_buff_t *skb_clone(sk_buff_t *skb)
{
	sk_buff_t *n;

	if (!skb)
		return NULL;
	BUG_ON(skb->pool_sig != SKB_HDR_MAGIC);
	n = hdr_take();
	if (!n) {
		stat_add(&st_clone_fail, 1);
		return NULL;
	}
	{
		uint8_t dyn = n->hdr_dynamic;

		mm_memcpy(n, skb, sizeof(*n));
		n->hdr_dynamic = dyn;
	}
	n->next = NULL;
	n->prev = NULL;
	n->ts_next = NULL;
	n->ts_prev = NULL;
	n->refcount = 1;
	n->pool_sig = SKB_HDR_MAGIC;
	n->destructor = NULL;
	n->sk = NULL;
	n->truesize = sizeof(sk_buff_t);
	n->cloned = 1;
	skb->cloned = 1;
	__atomic_fetch_add(&skb->shinfo->dataref, 1, __ATOMIC_ACQ_REL);
	stat_add(&st_inuse_bytes, sizeof(sk_buff_t));
	stat_add(&st_clone, 1);
	return n;
}

static void skb_copy_header(sk_buff_t *n, const sk_buff_t *o)
{
	n->dev = o->dev;
	n->protocol = o->protocol;
	n->ip_summed = o->ip_summed;
	n->pkt_type = o->pkt_type;
	n->csum = o->csum;
	n->hash = o->hash;
	n->tstamp_ns = o->tstamp_ns;
	n->src_ip = o->src_ip;
	n->dst_ip = o->dst_ip;
	n->ip_proto = o->ip_proto;
	n->rx_ttl = o->rx_ttl;
	mm_memcpy(n->cb, o->cb, SKB_CB_SIZE);
	n->shinfo->gso_segs = o->shinfo->gso_segs;
	n->shinfo->gso_size = o->shinfo->gso_size;
}

/* Header offsets of `o' carried over to `n' whose data starts `delta'
 * bytes further into its area. */
static void skb_copy_offsets(sk_buff_t *n, const sk_buff_t *o, int delta)
{
	if (o->mac_header != SKB_HDR_UNSET)
		n->mac_header = (uint16_t)(o->mac_header + delta);
	if (o->network_header != SKB_HDR_UNSET)
		n->network_header = (uint16_t)(o->network_header + delta);
	if (o->transport_header != SKB_HDR_UNSET)
		n->transport_header = (uint16_t)(o->transport_header + delta);
	if (o->ip_summed == CHECKSUM_PARTIAL)
		n->csum_start = (uint16_t)(o->csum_start + delta);
	n->csum_offset = o->csum_offset;
}

sk_buff_t *skb_copy_expand(const sk_buff_t *skb, uint32_t headroom,
			   uint32_t tailroom)
{
	uint32_t old_hr = skb_headroom(skb);
	sk_buff_t *n;

	if (headroom < old_hr)
		headroom = old_hr;
	if (headroom > SKB_JUMBO_DATA || tailroom > SKB_JUMBO_DATA)
		return NULL;
	n = skb_alloc_headroom(headroom, skb->len + tailroom);
	if (!n)
		return NULL;
	if (skb->len)
		mm_memcpy(skb_append(n, skb->len), skb->data, skb->len);
	skb_copy_header(n, skb);
	skb_copy_offsets(n, skb, (int)headroom - (int)old_hr);
	stat_add(&st_copy, 1);
	return n;
}

sk_buff_t *skb_copy(const sk_buff_t *skb)
{
	return skb_copy_expand(skb, skb_headroom(skb), 0);
}

sk_buff_t *skb_copy_fit(const sk_buff_t *skb)
{
	uint32_t hr = skb_headroom(skb);
	sk_buff_t *n = skb_alloc_fit(hr, skb->len);

	if (!n)
		return NULL;
	if (n->truesize >= skb->truesize) {
		skb_put(n);
		return NULL;
	}
	if (skb->len)
		mm_memcpy(skb_append(n, skb->len), skb->data, skb->len);
	skb_copy_header(n, skb);
	skb_copy_offsets(n, skb, 0);
	stat_add(&st_copy, 1);
	return n;
}

sk_buff_t *skb_unshare(sk_buff_t *skb)
{
	sk_buff_t *n;

	if (!skb || !skb_cloned(skb))
		return skb;
	n = skb_copy(skb);
	skb_put(skb);
	return n;
}

sk_buff_t *skb_realloc_headroom(sk_buff_t *skb, uint32_t headroom)
{
	sk_buff_t *n;

	if (!skb)
		return NULL;
	if (skb_headroom(skb) >= headroom && !skb_cloned(skb))
		return skb;
	n = skb_copy_expand(skb, headroom, 0);
	skb_put(skb);
	return n;
}

int skb_copy_bits(const sk_buff_t *skb, uint32_t off, void *to, uint32_t len)
{
	if (off > skb->len || len > skb->len - off)
		return -1;
	if (len)
		mm_memcpy(to, skb->data + off, len);
	return 0;
}

// ---- checksums ----

uint32_t csum_partial(const void *buf, uint32_t len, uint32_t sum)
{
	const uint8_t *p = buf;
	uint64_t acc = sum;

	while (len >= 2) {
		acc += (uint32_t)p[0] | ((uint32_t)p[1] << 8);
		p += 2;
		len -= 2;
	}
	if (len)
		acc += p[0];
	while (acc >> 32)
		acc = (acc & 0xFFFFFFFFu) + (acc >> 32);
	return (uint32_t)acc;
}

static inline uint32_t sw16(uint32_t v)
{
	return ((v & 0xFF) << 8) | ((v >> 8) & 0xFF);
}

uint32_t csum_tcpudp_nofold(uint32_t saddr, uint32_t daddr, uint16_t len,
			    uint8_t proto, uint32_t sum)
{
	uint64_t acc = sum;

	acc += sw16(saddr >> 16) + sw16(saddr & 0xFFFF);
	acc += sw16(daddr >> 16) + sw16(daddr & 0xFFFF);
	acc += sw16(proto) + sw16(len);
	while (acc >> 32)
		acc = (acc & 0xFFFFFFFFu) + (acc >> 32);
	return (uint32_t)acc;
}

uint32_t skb_checksum(const sk_buff_t *skb, uint32_t off, uint32_t len,
		      uint32_t sum)
{
	if (off > skb->len || len > skb->len - off)
		return sum;
	/* An odd offset would swap the bytes of every word; the stack only
	 * sums from header boundaries, which are even. */
	WARN_ON_ONCE(off & 1);
	return csum_partial(skb->data + off, len, sum);
}

// ---- reserve maintenance ----

void skb_get_mem_stats(skb_mem_stats_t *out)
{
	uint64_t f;

	mm_memset(out, 0, sizeof(*out));
	for (int c = 0; c < SKB_NR_CLS; c++) {
		struct skb_class *k = &classes[c];

		out->alloc[c] = st_alloc[c];
		spin_lock_irqsave(&k->lock, &f);
		out->static_free[c] = k->nstatic_free;
		out->reserve[c] = k->nreserve;
		out->dyn_slots[c] = k->ndyn;
		spin_unlock_irqrestore(&k->lock, f);
		out->static_total[c] = cls_count[c];
	}
	out->alloc_dynamic = st_alloc_dyn;
	out->promote = st_promote;
	out->fail = st_fail;
	out->clone = st_clone;
	out->clone_fail = st_clone_fail;
	out->copy = st_copy;
	out->dyn_bytes = st_dyn_bytes;
	out->dyn_limit = SKB_DYN_LIMIT;
	out->inuse_bytes = st_inuse_bytes;
	out->refill_runs = st_refill_runs;
	out->trim_frees = st_trim_frees;
}

/* Give one reserve slot of class `c' back to the slab, if there is one
 * beyond `keep'.  Returns 1 when it freed a slot. */
static int reserve_trim_one(int c, uint32_t keep)
{
	struct skb_class *k = &classes[c];
	skb_slot_t *s = NULL;
	uint64_t f;

	spin_lock_irqsave(&k->lock, &f);
	if (k->nreserve > keep && k->reserve) {
		s = k->reserve;
		k->reserve = s->free_next;
		k->nreserve--;
		k->ndyn--;
	}
	spin_unlock_irqrestore(&k->lock, f);
	if (!s)
		return 0;
	slab_free(s);
	stat_sub(&st_dyn_bytes, slot_bytes(c));
	stat_add(&st_trim_frees, 1);
	return 1;
}

static void hdr_trim(void)
{
	/* Dynamic clone headers beyond what the static pool could not
	 * cover are given back: walk the free list for dynamic ones while
	 * more headers are free than the static pool holds. */
	for (;;) {
		sk_buff_t *h = NULL, **pp;
		uint64_t f;

		spin_lock_irqsave(&hdr_lock, &f);
		if (hdr_ndyn && hdr_nfree > SKB_CLONE_HDRS / 2) {
			for (pp = &hdr_free; *pp; pp = &(*pp)->next) {
				if ((*pp)->hdr_dynamic) {
					h = *pp;
					*pp = h->next;
					hdr_nfree--;
					hdr_ndyn--;
					break;
				}
			}
		}
		spin_unlock_irqrestore(&hdr_lock, f);
		if (!h)
			return;
		slab_free(h);
	}
}

int skb_reserve_maintain(void)
{
	int again = 0;
	uint64_t now = timer_ticks();

	might_sleep();
	__atomic_store_n(&refill_kicked, 0, __ATOMIC_RELEASE);
	for (int c = 0; c < SKB_NR_CLS; c++) {
		struct skb_class *k = &classes[c];
		uint32_t sfree = __atomic_load_n(&k->nstatic_free,
						 __ATOMIC_RELAXED);
		int added = 0;

		if (sfree < res_low[c]) {
			relax_since[c] = 0;
			/* Top the reserve up (bounded per run). */
			for (int i = 0; i < 64; i++) {
				skb_slot_t *s;
				uint64_t f;

				if (__atomic_load_n(&k->nreserve,
						    __ATOMIC_RELAXED) >=
				    res_target[c])
					break;
				s = slot_new_dynamic(c);
				if (!s)
					break;
				spin_lock_irqsave(&k->lock, &f);
				s->free_next = k->reserve;
				k->reserve = s;
				k->nreserve++;
				spin_unlock_irqrestore(&k->lock, f);
				added++;
			}
			if (added) {
				stat_add(&st_refill_runs, 1);
				if (__atomic_load_n(&k->nreserve,
						    __ATOMIC_RELAXED) <
				    res_target[c])
					again = 1;
			}
		} else if (c >= SKB_CLS_FIT0) {
			/* A fitted class relaxes when it has not been used for
			 * a while (it has no static pool to watch). */
			uint64_t used = __atomic_load_n(&fit_last_use[c],
							__ATOMIC_RELAXED);
			uint64_t since = used && used <= now ? used : now;

			relax_since[c] = since ? since : 1;
		} else if (sfree >= res_relax[c] ||
			   sfree == cls_count[c]) {
			if (!relax_since[c])
				relax_since[c] = now ? now : 1;
		}
		/* Excess beyond the high mark goes back at once. */
		for (int i = 0; i < 256 && reserve_trim_one(c, res_high[c]);
		     i++)
			;
		/* The whole reserve once the pressure has been gone a
		 * while. */
		if (relax_since[c] &&
		    now - relax_since[c] >= timer_ms_to_ticks(SKB_RELAX_MS)) {
			for (int i = 0; i < 256 && reserve_trim_one(c, 0); i++)
				;
			if (__atomic_load_n(&k->nreserve, __ATOMIC_RELAXED))
				again = 1;
		}
	}
	hdr_trim();
	return again;
}

// ---- queues ----

void skb_queue_init(skb_queue_t *q, const char *name)
{
	BUG_ON(q == NULL);
	q->head = NULL;
	q->tail = NULL;
	q->len = 0;
	spinlock_init(&q->lock, name);
}

void __skb_queue_tail(skb_queue_t *q, sk_buff_t *skb)
{
	skb->next = NULL;
	skb->prev = q->tail;
	if (q->tail)
		q->tail->next = skb;
	else
		q->head = skb;
	q->tail = skb;
	__atomic_store_n(&q->len, q->len + 1, __ATOMIC_RELEASE);
}

void __skb_queue_head(skb_queue_t *q, sk_buff_t *skb)
{
	skb->prev = NULL;
	skb->next = q->head;
	if (q->head)
		q->head->prev = skb;
	else
		q->tail = skb;
	q->head = skb;
	__atomic_store_n(&q->len, q->len + 1, __ATOMIC_RELEASE);
}

void __skb_unlink(sk_buff_t *skb, skb_queue_t *q)
{
	BUG_ON(q->len == 0);
	if (skb->prev)
		skb->prev->next = skb->next;
	else
		q->head = skb->next;
	if (skb->next)
		skb->next->prev = skb->prev;
	else
		q->tail = skb->prev;
	skb->next = NULL;
	skb->prev = NULL;
	__atomic_store_n(&q->len, q->len - 1, __ATOMIC_RELEASE);
	WARN_ON((q->head == NULL) != (q->len == 0));
}

sk_buff_t *__skb_dequeue(skb_queue_t *q)
{
	sk_buff_t *skb = q->head;

	if (skb)
		__skb_unlink(skb, q);
	return skb;
}

sk_buff_t *__skb_dequeue_tail(skb_queue_t *q)
{
	sk_buff_t *skb = q->tail;

	if (skb)
		__skb_unlink(skb, q);
	return skb;
}

void skb_queue_tail(skb_queue_t *q, sk_buff_t *skb)
{
	uint64_t f;

	if (!skb)
		return;
	spin_lock_irqsave(&q->lock, &f);
	__skb_queue_tail(q, skb);
	spin_unlock_irqrestore(&q->lock, f);
}

void skb_queue_head(skb_queue_t *q, sk_buff_t *skb)
{
	uint64_t f;

	if (!skb)
		return;
	spin_lock_irqsave(&q->lock, &f);
	__skb_queue_head(q, skb);
	spin_unlock_irqrestore(&q->lock, f);
}

sk_buff_t *skb_dequeue(skb_queue_t *q)
{
	sk_buff_t *skb;
	uint64_t f;

	spin_lock_irqsave(&q->lock, &f);
	skb = __skb_dequeue(q);
	spin_unlock_irqrestore(&q->lock, f);
	return skb;
}

sk_buff_t *skb_dequeue_tail(skb_queue_t *q)
{
	sk_buff_t *skb;
	uint64_t f;

	spin_lock_irqsave(&q->lock, &f);
	skb = __skb_dequeue_tail(q);
	spin_unlock_irqrestore(&q->lock, f);
	return skb;
}

void skb_unlink(sk_buff_t *skb, skb_queue_t *q)
{
	uint64_t f;

	spin_lock_irqsave(&q->lock, &f);
	__skb_unlink(skb, q);
	spin_unlock_irqrestore(&q->lock, f);
}

void skb_queue_splice_tail_init(skb_queue_t *from, skb_queue_t *to)
{
	sk_buff_t *h, *t;
	uint32_t n;
	uint64_t f1, f2;

	if (from == to)
		return;
	spin_lock_irqsave(&from->lock, &f1);
	h = from->head;
	t = from->tail;
	n = from->len;
	from->head = from->tail = NULL;
	__atomic_store_n(&from->len, 0, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&from->lock, f1);
	if (!h)
		return;
	spin_lock_irqsave(&to->lock, &f2);
	h->prev = to->tail;
	if (to->tail)
		to->tail->next = h;
	else
		to->head = h;
	to->tail = t;
	__atomic_store_n(&to->len, to->len + n, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&to->lock, f2);
}

sk_buff_t *skb_queue_take_all(skb_queue_t *q, uint32_t *count)
{
	sk_buff_t *h;
	uint64_t f;

	spin_lock_irqsave(&q->lock, &f);
	h = q->head;
	if (count)
		*count = q->len;
	q->head = q->tail = NULL;
	__atomic_store_n(&q->len, 0, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&q->lock, f);
	return h;
}

void skb_queue_purge(skb_queue_t *q)
{
	sk_buff_t *skb = skb_queue_take_all(q, NULL);

	while (skb) {
		sk_buff_t *next = skb->next;

		skb->next = skb->prev = NULL;
		skb_put(skb);
		skb = next;
	}
}
