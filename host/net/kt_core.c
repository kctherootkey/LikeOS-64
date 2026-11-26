/* Kernel-side probes for the core tests of the network harness (skb,
 * softirq loop, timer wheel, FIB/dst, NAPI, transmit queue, neighbours).
 *
 * Compiled into every stack instance next to the real kernel/net sources
 * (host/test-net.sh), so these functions call the stack's internals
 * directly; after the per-instance rename the host side calls them as
 * n0_kt_*().  Every check function returns 0 on success, otherwise the
 * line of the failed check, with a message in kt_msg.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/net/net.h>
#include <kernel/net/skb.h>
#include <kernel/net/softirq.h>
#include <kernel/net/softirq_core.h>
#include <kernel/net/net_timer.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/neighbour.h>
#include <kernel/net/fib.h>
#include <kernel/net/dst.h>
#include <kernel/net/ip.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include "kstubs.h"

char kt_msg[256];

#define KT(cond, ...)                                                   \
	do {                                                            \
		if (!(cond)) {                                          \
			ksnprintf(kt_msg, sizeof(kt_msg), __VA_ARGS__); \
			return __LINE__;                                \
		}                                                       \
	} while (0)

extern struct neigh_table arp_tbl;

/* ------------------------------------------------------------------ */
/* skb                                                                  */
/* ------------------------------------------------------------------ */

int kt_skb_basic(void)
{
	sk_buff_t *skb = skb_alloc(100);
	uint8_t *p;

	KT(skb, "alloc");
	KT(skb_headroom(skb) == SKB_HEADROOM, "headroom %u", skb_headroom(skb));
	KT(skb_tailroom(skb) >= 100 && skb->len == 0 && skb->refcount == 1,
	   "fresh skb");
	KT(skb->shinfo->dataref == 1 && !skb_cloned(skb), "dataref");
	p = skb_append(skb, 60);
	for (int i = 0; i < 60; i++)
		p[i] = (uint8_t)i;
	KT(skb->len == 60 && skb->tail == skb->data + 60, "append");
	p = skb_push(skb, 14);
	KT(p == skb->data && skb->len == 74 &&
		   skb_headroom(skb) == SKB_HEADROOM - 14,
	   "push");
	KT(skb_pull(skb, 14) && skb->data[0] == 0 && skb->len == 60, "pull");
	KT(skb_pull(skb, 61) == NULL && skb->len == 60, "pull past the end");
	skb_trim(skb, 20);
	KT(skb->len == 20 && skb->tail == skb->data + 20, "trim");
	skb_trim(skb, 50);
	KT(skb->len == 20, "trim longer is a no-op");
	skb_reset_network_header(skb);
	KT(skb_network_header(skb) == skb->data, "network header");
	{
		uint8_t b[8];

		KT(skb_copy_bits(skb, 4, b, 8) == 0 && b[0] == 4 && b[7] == 11,
		   "copy_bits");
		KT(skb_copy_bits(skb, 15, b, 8) < 0, "copy_bits range");
	}
	KT(skb_get(skb) == skb && skb->refcount == 2, "get");
	skb_put(skb);
	KT(skb->refcount == 1, "put of a shared header");
	skb_put(skb);
	{
		sk_buff_t *h = skb_alloc_headroom(16, 40);

		KT(h && skb_headroom(h) == 16, "alloc_headroom");
		skb_reserve(h, 4);
		KT(skb_headroom(h) == 20 && h->len == 0, "reserve");
		skb_put(h);
	}
	KT(skb_alloc(SKB_MAX_ALLOC + 1) == NULL, "oversized request");
	{
		sk_buff_t *j = skb_alloc(9000);

		KT(j && j->shinfo->cls == SKB_CLS_JUMBO, "jumbo class");
		skb_put(j);
	}
	return 0;
}

/* The fitted classes: the smallest class that holds a request, a copy
 * into one only when it costs less, the memory accounting balanced. */
int kt_skb_fit(void)
{
	static const uint32_t want[] = { 100,  1500,  3000,  6000,  9000,
					 12000, 20000, 30000, 45000, 60000 };
	skb_mem_stats_t m0, m1;
	sk_buff_t *j, *n;

	skb_get_mem_stats(&m0);
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		sk_buff_t *s = skb_alloc_fit(64, want[i]);
		uint32_t room;

		KT(s, "fit %u: no buffer", want[i]);
		room = (uint32_t)(s->end - s->head);
		KT(room >= 64 + want[i], "fit %u: room %u", want[i], room);
		/* at most twice what was asked, or one page beyond it */
		KT(s->truesize <= 2 * (64 + want[i]) + 1024 ||
			   s->truesize <= 64 + want[i] + 4096,
		   "fit %u: truesize %u", want[i], s->truesize);
		KT(want[i] + 64 <= SKB_SMALL_DATA ?
			   s->shinfo->cls == SKB_CLS_SMALL :
		   want[i] > 50000 ? s->shinfo->cls == SKB_CLS_JUMBO :
				      s->shinfo->cls >= SKB_CLS_FIT0,
		   "fit %u: class %u", want[i], s->shinfo->cls);
		mm_memset(skb_append(s, want[i]), 0x33, want[i]);
		skb_put(s);
	}
	// a jumbo buffer holding 3000 bytes moves into a one-page slot
	j = skb_alloc_headroom(64, 65000);
	KT(j && j->shinfo->cls == SKB_CLS_JUMBO, "jumbo");
	for (uint32_t i = 0; i < 3000; i++)
		*skb_append(j, 1) = (uint8_t)i;
	j->protocol = 0x0800;
	j->cb[5] = 0x77;
	skb_reset_network_header(j);
	n = skb_copy_fit(j);
	KT(n && n->len == 3000 && n->truesize < j->truesize / 8,
	   "copy_fit: %p truesize %u", (void *)n, n ? n->truesize : 0);
	KT(n->data[2999] == (uint8_t)2999 && n->protocol == 0x0800 &&
		   n->cb[5] == 0x77 && skb_headroom(n) == skb_headroom(j) &&
		   skb_network_header(n) == n->data,
	   "copy_fit contents");
	skb_put(n);
	// a buffer that already fits is not copied
	n = skb_alloc_fit(64, 3000);
	KT(n, "fit 3000");
	skb_append(n, 3000);
	KT(skb_copy_fit(n) == NULL, "copied a buffer that already fits");
	skb_put(n);
	skb_put(j);
	skb_get_mem_stats(&m1);
	KT(m1.inuse_bytes == m0.inuse_bytes, "in-use accounting %llu -> %llu",
	   (unsigned long long)m0.inuse_bytes,
	   (unsigned long long)m1.inuse_bytes);
	KT(m1.reserve[SKB_CLS_FIT0] > 0, "released fitted slots not kept");
	return 0;
}

int kt_skb_clone(void)
{
	skb_mem_stats_t m0, m1;
	sk_buff_t *a, *c, *u, *x;

	skb_get_mem_stats(&m0);
	a = skb_alloc(200);
	KT(a, "alloc");
	mm_memset(skb_append(a, 200), 0x5a, 200);
	c = skb_clone(a);
	KT(c && c != a && c->data == a->data && c->len == 200, "clone");
	KT(a->shinfo->dataref == 2 && skb_cloned(a) && skb_cloned(c),
	   "dataref 2");
	// a private copy for the writer
	u = skb_unshare(c);
	KT(u && u != c && u->data != a->data, "unshare copies a clone");
	KT(a->shinfo->dataref == 1 && !skb_cloned(a), "back to one user");
	u->data[0] = 1;
	KT(a->data[0] == 0x5a, "the copy is private");
	KT(skb_unshare(a) == a, "unshare of a private skb");
	// release the owner first, the clone keeps the data
	c = skb_clone(a);
	KT(c, "clone 2");
	skb_put(a);
	KT(c->data[199] == 0x5a && c->shinfo->dataref == 1, "data survives");
	x = skb_copy_expand(c, 200, 50);
	KT(x && skb_headroom(x) >= 200 && skb_tailroom(x) >= 50 &&
		   x->len == 200 && x->data[10] == 0x5a,
	   "copy_expand");
	x = skb_realloc_headroom(x, 64);
	KT(x && skb_headroom(x) >= 64, "realloc_headroom keeps a fit skb");
	skb_put(x);
	skb_put(c);
	skb_put(u);
	skb_get_mem_stats(&m1);
	KT(m1.static_free[SKB_CLS_SMALL] == m0.static_free[SKB_CLS_SMALL],
	   "small slots leaked: %u -> %u", m0.static_free[SKB_CLS_SMALL],
	   m1.static_free[SKB_CLS_SMALL]);
	KT(m1.inuse_bytes == m0.inuse_bytes, "in-use accounting %llu -> %llu",
	   (unsigned long long)m0.inuse_bytes,
	   (unsigned long long)m1.inuse_bytes);
	KT(m1.clone >= m0.clone + 2, "clone count");
	return 0;
}

int kt_skb_queue(void)
{
	skb_queue_t q, r;
	sk_buff_t *s[6], *x;
	uint32_t n;

	skb_queue_init(&q, "kt_q");
	skb_queue_init(&r, "kt_r");
	for (int i = 0; i < 6; i++) {
		s[i] = skb_alloc(10);
		KT(s[i], "alloc");
		s[i]->cb[0] = (uint8_t)i;
	}
	skb_queue_tail(&q, s[1]);
	skb_queue_tail(&q, s[2]);
	skb_queue_head(&q, s[0]);
	KT(skb_queue_len(&q) == 3 && skb_peek(&q) == s[0] &&
		   skb_peek_tail(&q) == s[2],
	   "order");
	skb_unlink(s[1], &q);
	KT(skb_queue_len(&q) == 2 && s[0]->next == s[2] && s[2]->prev == s[0],
	   "unlink middle");
	skb_queue_tail(&r, s[3]);
	skb_queue_tail(&r, s[4]);
	skb_queue_splice_tail_init(&r, &q);
	KT(skb_queue_empty(&r) && skb_queue_len(&q) == 4 &&
		   skb_peek_tail(&q) == s[4],
	   "splice");
	KT(skb_dequeue_tail(&q) == s[4], "dequeue_tail");
	KT(skb_dequeue(&q) == s[0], "dequeue");
	x = skb_queue_take_all(&q, &n);
	KT(n == 2 && x == s[2] && x->next == s[3] && skb_queue_empty(&q),
	   "take_all");
	skb_put(s[0]);
	skb_put(s[4]);
	skb_put(s[2]);
	skb_put(s[3]);
	skb_queue_tail(&q, s[1]);
	skb_queue_tail(&q, s[5]);
	skb_queue_purge(&q);
	KT(skb_queue_empty(&q) && skb_dequeue(&q) == NULL, "purge");
	return 0;
}

int kt_skb_csum(void)
{
	/* a UDP datagram 10.0.0.1:1000 -> 10.0.0.2:2000, 4 bytes "abcd" */
	uint8_t udp[12] = { 0x03, 0xe8, 0x07, 0xd0, 0x00, 0x0c, 0, 0,
			    'a',  'b',	'c',  'd' };
	uint32_t s = csum_tcpudp_nofold(0x0a000001, 0x0a000002, 12, 17,
					csum_partial(udp, 12, 0));
	uint16_t c = csum_fold(s);

	mm_memcpy(udp + 6, &c, 2);
	s = csum_tcpudp_nofold(0x0a000001, 0x0a000002, 12, 17,
			       csum_partial(udp, 12, 0));
	KT(csum_fold(s) == 0, "a datagram with its own checksum sums to 0");
	{
		uint8_t hdr[20] = { 0x45, 0, 0, 40, 0, 1, 0x40, 0, 64, 6,
				    0, 0, 10, 0, 0, 1, 10, 0, 0, 2 };

		KT(csum_fold(csum_partial(hdr, 20, 0)) ==
			   ipv4_checksum(hdr, 20),
		   "csum_partial agrees with ipv4_checksum");
	}
	return 0;
}

/* Exhaust the static small pool: dynamic slots with interrupts on, the
 * reserve with them off, nothing beyond. */
static sk_buff_t *kt_hold[4096];
static int kt_nhold;

int kt_skb_pool(void)
{
	skb_mem_stats_t m;
	sk_buff_t *skb;
	uint64_t f;

	skb_get_mem_stats(&m);
	kt_nhold = 0;
	while (kt_nhold < (int)m.static_total[SKB_CLS_SMALL] + 64) {
		skb = skb_alloc(100);
		KT(skb, "allocation %d failed with interrupts on", kt_nhold);
		kt_hold[kt_nhold++] = skb;
	}
	skb_get_mem_stats(&m);
	KT(m.static_free[SKB_CLS_SMALL] == 0 && m.alloc_dynamic >= 64 &&
		   m.dyn_slots[SKB_CLS_SMALL] >= 64,
	   "dynamic slots: free %u dyn %llu", m.static_free[SKB_CLS_SMALL],
	   (unsigned long long)m.alloc_dynamic);
	// interrupts off: no slab; the jumbo pool takes small requests
	f = local_irq_save();
	skb = skb_alloc(100);
	local_irq_restore(f);
	KT(skb && skb->shinfo->cls == SKB_CLS_JUMBO, "promotion");
	skb_put(skb);
	// the housekeeping refill tops the reserve up (interrupts on)
	KT(skb_reserve_maintain() >= 0, "maintain");
	skb_get_mem_stats(&m);
	KT(m.reserve[SKB_CLS_SMALL] > 0, "no reserve after a refill");
	f = local_irq_save();
	skb = skb_alloc(100);
	local_irq_restore(f);
	KT(skb && skb->shinfo->cls == SKB_CLS_SMALL && skb->shinfo->dynamic,
	   "an interrupts-off allocation from the reserve");
	kt_hold[kt_nhold++] = skb;
	return 0;
}

/* Give everything back; the reserve must survive until the pressure has
 * been gone a while (second step: kt_skb_pool_relaxed). */
int kt_skb_pool_release(void)
{
	skb_mem_stats_t m;

	for (int i = 0; i < kt_nhold; i++)
		skb_put(kt_hold[i]);
	kt_nhold = 0;
	skb_get_mem_stats(&m);
	KT(m.static_free[SKB_CLS_SMALL] == m.static_total[SKB_CLS_SMALL],
	   "static slots back: %u of %u", m.static_free[SKB_CLS_SMALL],
	   m.static_total[SKB_CLS_SMALL]);
	KT(m.reserve[SKB_CLS_SMALL] == m.dyn_slots[SKB_CLS_SMALL],
	   "every dynamic slot parked on the reserve");
	return 0;
}

int kt_skb_dyn_slots(void)
{
	skb_mem_stats_t m;

	skb_reserve_maintain();
	skb_get_mem_stats(&m);
	return (int)m.dyn_slots[SKB_CLS_SMALL];
}

/* ------------------------------------------------------------------ */
/* softirq loop policy                                                  */
/* ------------------------------------------------------------------ */

static volatile uint32_t sl_pending;
static int sl_reraise_a, sl_order[64], sl_norder;
static uint64_t sl_clock;

static void sl_a(void)
{
	sl_order[sl_norder++ & 63] = 0;
	if (sl_reraise_a > 0) {
		sl_reraise_a--;
		sl_pending |= 1u << 0;
	}
}

static void sl_c(void)
{
	sl_order[sl_norder++ & 63] = 3;
}

static uint64_t sl_now(void)
{
	return sl_clock += 1000000; // 1 ms per call
}

static uint64_t sl_now_still(void)
{
	return 0;
}

static int sl_yield_flag;
static int sl_should_yield(void)
{
	return sl_yield_flag;
}

static void sl_nop(void)
{
}

int kt_softirq_loop(void)
{
	softirq_loop_fn_t h[32] = { 0 };
	uint64_t counts[32] = { 0 };
	struct softirq_loop l = { 0 };
	uint32_t left;

	h[0] = sl_a;
	h[3] = sl_c;
	l.pending = &sl_pending;
	l.handlers = h;
	l.counts = counts;
	l.now_ns = sl_now_still;
	l.irq_enable = sl_nop;
	l.irq_disable = sl_nop;
	l.time_limit_ns = MAX_SOFTIRQ_TIME_NS;
	l.max_restart = MAX_SOFTIRQ_RESTART;

	// drains when nothing re-raises; lowest vector first
	sl_pending = (1u << 3) | (1u << 0);
	sl_norder = 0;
	left = softirq_loop_run(&l);
	KT(left == 0 && sl_pending == 0 && !l.stopped_by_limit, "drain");
	KT(sl_norder == 2 && sl_order[0] == 0 && sl_order[1] == 3, "order");
	KT(counts[0] == 1 && counts[3] == 1, "counts");

	// a vector that re-raises itself: MAX_SOFTIRQ_RESTART passes
	sl_reraise_a = 1000;
	sl_pending = 1;
	sl_norder = 0;
	left = softirq_loop_run(&l);
	KT(left == 1 && l.stopped_by_limit && sl_norder == MAX_SOFTIRQ_RESTART,
	   "restart limit: %d passes, left %x", sl_norder, left);
	KT(l.restarts == MAX_SOFTIRQ_RESTART - 1, "restarts %d", l.restarts);

	// the time limit (the clock moves 1 ms per reading)
	l.now_ns = sl_now;
	sl_clock = 0;
	sl_pending = 1;
	sl_norder = 0;
	left = softirq_loop_run(&l);
	KT(left && l.stopped_by_limit && sl_norder == 2,
	   "time limit after %d passes", sl_norder);

	// a due reschedule stops after one pass
	l.now_ns = sl_now_still;
	sl_yield_flag = 1;
	l.should_yield = sl_should_yield;
	sl_pending = 1;
	sl_norder = 0;
	left = softirq_loop_run(&l);
	KT(left && sl_norder == 1, "should_yield");
	sl_yield_flag = 0;
	sl_reraise_a = 0;
	sl_pending = 1;
	left = softirq_loop_run(&l);
	KT(left == 0, "drained at last");
	return 0;
}

/* ------------------------------------------------------------------ */
/* net timer wheel                                                      */
/* ------------------------------------------------------------------ */

static net_timer_t tt[6];
static uint64_t tt_fired[6], tt_armed_at;
static int tt_count[6];

static void tt_fn(net_timer_t *t)
{
	int i = (int)(t - tt);

	tt_fired[i] = timer_ticks();
	tt_count[i]++;
	if (i == 4 && tt_count[4] < 3) // re-arms itself for "now"
		net_timer_mod(t, timer_ticks());
}

int kt_timer_arm(void)
{
	uint64_t now = timer_ticks();

	tt_armed_at = now;
	for (int i = 0; i < 6; i++) {
		net_timer_setup(&tt[i], tt_fn);
		tt_fired[i] = 0;
		tt_count[i] = 0;
	}
	net_timer_mod(&tt[0], now + 1);
	net_timer_mod(&tt[1], now + 10);
	net_timer_mod(&tt[2], now + 600); // beyond one wheel turn
	net_timer_mod(&tt[3], now + 20);
	KT(net_timer_del(&tt[3]) == 1 && !net_timer_pending(&tt[3]), "del");
	net_timer_mod(&tt[4], now - 5); // in the past: the next run
	net_timer_mod(&tt[5], now + 30);
	KT(net_timer_mod(&tt[5], now + 40) == 1, "mod of a pending timer");
	KT(net_timer_armed() >= 5, "armed %u", net_timer_armed());
	return 0;
}

int kt_timer_check(void)
{
	uint64_t a = tt_armed_at;

	KT(tt_count[0] == 1 && tt_fired[0] == a + 1, "+1 fired at %llu",
	   (unsigned long long)(tt_fired[0] - a));
	KT(tt_count[1] == 1 && tt_fired[1] == a + 10, "+10 at %llu",
	   (unsigned long long)(tt_fired[1] - a));
	KT(tt_count[2] == 1 && tt_fired[2] == a + 600, "+600 at %llu",
	   (unsigned long long)(tt_fired[2] - a));
	KT(tt_count[3] == 0, "a deleted timer fired");
	KT(tt_count[4] == 3, "self re-arming timer ran %d times", tt_count[4]);
	KT(tt_count[5] == 1 && tt_fired[5] == a + 40, "moved timer at %llu",
	   (unsigned long long)(tt_fired[5] - a));
	return 0;
}

/* ------------------------------------------------------------------ */
/* FIB and dst                                                          */
/* ------------------------------------------------------------------ */

static struct fib_config kt_cfg(uint32_t dst, uint32_t mask, uint32_t gw,
				uint32_t metric)
{
	struct fib_config c;

	mm_memset(&c, 0, sizeof(c));
	c.dst = dst;
	c.mask = mask;
	c.gw = gw;
	c.dev = net_get_default_device();
	c.metric = metric;
	c.flags = RTF_UP | (gw ? RTF_GATEWAY : 0);
	return c;
}

int kt_fib_lpm(void)
{
	struct fib_config c;
	struct fib_result r;
	uint32_t base = fib_route_count();

	c = kt_cfg(IP4_ADDR(192, 168, 0, 0), 0xFFFF0000, IP4_ADDR(10, 0, 0, 2),
		   0);
	KT(fib_table_insert(&c) == 0, "insert /16");
	c = kt_cfg(IP4_ADDR(192, 168, 7, 0), 0xFFFFFF00, IP4_ADDR(10, 0, 0, 3),
		   0);
	KT(fib_table_insert(&c) == 0, "insert /24");
	c = kt_cfg(IP4_ADDR(192, 168, 7, 9), 0xFFFFFFFF, IP4_ADDR(10, 0, 0, 4),
		   0);
	KT(fib_table_insert(&c) == 0, "insert /32");
	c = kt_cfg(IP4_ADDR(192, 168, 7, 0), 0xFFFFFF80, 0, 0);
	KT(fib_table_insert(&c) == 0, "insert /25");
	c = kt_cfg(IP4_ADDR(1, 2, 3, 0), 0xFF00FF00, 0, 0);
	KT(fib_table_insert(&c) == -EINVAL, "non-contiguous mask accepted");
	c = kt_cfg(IP4_ADDR(1, 2, 3, 4), 0xFFFFFF00, 0, 0);
	KT(fib_table_insert(&c) == -EINVAL, "host bits accepted");
	KT(fib_route_count() == base + 4, "count %u", fib_route_count());

	KT(fib_lookup(IP4_ADDR(192, 168, 7, 9), &r) == 0 && r.plen == 32 &&
		   r.gw == IP4_ADDR(10, 0, 0, 4),
	   "/32 wins");
	KT(fib_lookup(IP4_ADDR(192, 168, 7, 10), &r) == 0 && r.plen == 25 &&
		   r.gw == 0,
	   "/25 wins over /24: plen %u", r.plen);
	KT(fib_lookup(IP4_ADDR(192, 168, 7, 200), &r) == 0 && r.plen == 24 &&
		   r.gw == IP4_ADDR(10, 0, 0, 3),
	   "/24");
	KT(fib_lookup(IP4_ADDR(192, 168, 9, 1), &r) == 0 && r.plen == 16,
	   "/16");
	KT(fib_lookup(IP4_ADDR(10, 0, 0, 77), &r) == 0 && r.plen == 24,
	   "the on-link subnet");
	// the same prefix again replaces the route
	c = kt_cfg(IP4_ADDR(192, 168, 7, 0), 0xFFFFFF00, IP4_ADDR(10, 0, 0, 5),
		   7);
	KT(fib_table_insert(&c) == 0 && fib_route_count() == base + 4,
	   "replace");
	KT(fib_lookup(IP4_ADDR(192, 168, 7, 200), &r) == 0 &&
		   r.gw == IP4_ADDR(10, 0, 0, 5) && r.metric == 7,
	   "replaced route");
	KT(fib_table_delete(IP4_ADDR(192, 168, 7, 0), 0xFFFFFF00,
			    IP4_ADDR(10, 0, 0, 9)) == -ESRCH,
	   "delete with another gateway");
	KT(fib_table_delete(IP4_ADDR(192, 168, 7, 0), 0xFFFFFF80, 0) == 0,
	   "delete /25");
	KT(fib_lookup(IP4_ADDR(192, 168, 7, 10), &r) == 0 && r.plen == 24,
	   "after the delete the /24 covers it");
	// a default route catches the rest; without one: unreachable
	KT(fib_lookup(IP4_ADDR(8, 8, 8, 8), &r) == -ENETUNREACH ||
		   r.plen == 0,
	   "default");
	// reject route
	KT(route_add(IP4_ADDR(172, 16, 0, 0), 0xFFF00000, 0, NULL, 0,
		     RTF_UP | RTF_REJECT) == 0,
	   "reject route");
	{
		struct dst_entry *d = NULL;

		KT(ip_route_output(IP4_ADDR(172, 16, 1, 1), 0, NULL, &d) ==
				   -EHOSTUNREACH &&
			   d == NULL,
		   "reject route output");
		KT(route_lookup(IP4_ADDR(172, 16, 1, 1), NULL) == NULL,
		   "route_lookup through a reject route");
	}
	KT(route_add(1, 0, 0, NULL, 0, RTF_UP | RTF_GATEWAY) == -EINVAL,
	   "gateway route without gateway");
	return 0;
}

int kt_fib_dst(void)
{
	struct dst_entry *a = NULL, *b = NULL, *c = NULL, *l = NULL,
			 *bc = NULL;
	struct dst_cache cache = { 0 };
	struct dst_stats s0, s1;
	net_device_t *dev = net_get_default_device();
	uint32_t g;

	dst_get_stats(&s0);
	KT(ip_route_output(IP4_ADDR(10, 0, 0, 77), 0, NULL, &a) == 0 && a &&
		   a->dev == dev && a->gateway == 0 &&
		   rt_nexthop(a, IP4_ADDR(10, 0, 0, 77)) ==
			   IP4_ADDR(10, 0, 0, 77),
	   "on-link route");
	KT(ip_route_output(IP4_ADDR(10, 0, 0, 78), 0, NULL, &b) == 0 && b == a,
	   "one entry per route, shared");
	KT(dst_mtu(a) == dev->mtu && dst_advmss(a) == dev->mtu - 40u &&
		   dst_hoplimit(a) == 64,
	   "metrics mtu %u advmss %u", dst_mtu(a), dst_advmss(a));
	KT(ip_route_output(dev->ip_addr, 0, NULL, &l) == 0 &&
		   l->type == RTN_LOCAL && l->dev == net_get_loopback(),
	   "local address");
	KT(ip_route_output(0xFFFFFFFFu, 0, NULL, &bc) == 0 &&
		   bc->type == RTN_BROADCAST && bc->dev == dev,
	   "limited broadcast");
	dst_release(bc);
	bc = NULL;
	KT(ip_route_output(dev->ip_addr | ~dev->netmask, 0, NULL, &bc) == 0 &&
		   bc->type == RTN_BROADCAST,
	   "subnet broadcast");
	KT(ip_route_output(IP4_ADDR(127, 0, 0, 1), 0, NULL, &c) == 0 &&
		   c->dev == net_get_loopback(),
	   "127.0.0.1");
	dst_release(c);
	// the socket cache keeps it while the table does not change
	dst_cache_set(&cache, a, IP4_ADDR(10, 0, 0, 77));
	c = dst_cache_get(&cache);
	KT(c == a, "cache hit");
	dst_release(c);
	g = fib_genid();
	KT(route_add(IP4_ADDR(192, 0, 2, 0), 0xFFFFFF00, IP4_ADDR(10, 0, 0, 2),
		     dev, 0, RTF_UP | RTF_GATEWAY) == 0,
	   "route add");
	KT(fib_genid() != g, "a table change bumps the generation");
	KT(dst_check(a) == NULL && dst_cache_get(&cache) == NULL,
	   "old entries are invalid after a change");
	KT(ip_route_output(IP4_ADDR(10, 0, 0, 77), 0, NULL, &c) == 0 && c != a &&
		   dst_valid(c),
	   "a fresh entry");
	dst_release(c);
	dst_release(a);
	dst_release(b);
	dst_release(l);
	dst_release(bc);
	dst_cache_reset(&cache);
	// gateway route
	KT(ip_route_output(IP4_ADDR(192, 0, 2, 50), 0, NULL, &a) == 0 &&
		   a->gateway == IP4_ADDR(10, 0, 0, 2) &&
		   rt_nexthop(a, IP4_ADDR(192, 0, 2, 50)) ==
			   IP4_ADDR(10, 0, 0, 2),
	   "gateway next hop");
	dst_release(a);
	KT(route_del(IP4_ADDR(192, 0, 2, 0), 0xFFFFFF00, 0) == 0, "route del");
	dst_get_stats(&s1);
	/* Every entry not owned by a route or the local/broadcast cache is
	 * back: at most a handful are still cached. */
	KT(s1.pool_free + 8 >= s0.pool_free, "dst leak: free %u -> %u",
	   s0.pool_free, s1.pool_free);
	return 0;
}

int kt_fib_exc_set(void)
{
	uint32_t d = IP4_ADDR(10, 0, 0, 2);
	struct dst_entry *a = NULL;

	KT(pmtu_get(d) == 0, "no exception yet");
	pmtu_set(d, 1400);
	KT(pmtu_get(d) == 1400, "pmtu %u", pmtu_get(d));
	pmtu_set(d, 1450);
	KT(pmtu_get(d) == 1400, "a report never raises it");
	pmtu_set(d, 300);
	KT(pmtu_get(d) == FIB_MIN_PMTU, "floor: %u", pmtu_get(d));
	pmtu_set(IP4_ADDR(10, 0, 0, 3), 1600);
	KT(pmtu_get(IP4_ADDR(10, 0, 0, 3)) == 0,
	   "a report above the device MTU");
	KT(ip_route_output(d, 0, NULL, &a) == 0 && dst_mtu(a) == FIB_MIN_PMTU &&
		   (a->flags & DST_HOST),
	   "the exception's entry: mtu %u", a ? dst_mtu(a) : 0);
	dst_release(a);
	a = NULL;
	KT(ip_route_output(IP4_ADDR(10, 0, 0, 4), 0, NULL, &a) == 0 &&
		   dst_mtu(a) == net_get_default_device()->mtu,
	   "other destinations keep the device MTU");
	dst_release(a);
	return 0;
}

int kt_fib_exc_expired(void)
{
	uint32_t d = IP4_ADDR(10, 0, 0, 2);
	struct dst_entry *a = NULL;

	KT(pmtu_get(d) == 0, "the exception outlived 600 s: %u", pmtu_get(d));
	KT(ip_route_output(d, 0, NULL, &a) == 0 &&
		   dst_mtu(a) == net_get_default_device()->mtu,
	   "the route MTU is back");
	dst_release(a);
	return 0;
}

/* ------------------------------------------------------------------ */
/* NAPI                                                                 */
/* ------------------------------------------------------------------ */

struct kt_napi {
	struct napi_struct napi;
	int left; // frames still to "receive"
	int done; // completed
	int polls;
	int max_budget_seen;
};
#define KN 7
static struct kt_napi kn[KN];
static int kn_order[256], kn_norder;
static uint64_t kn_run_of[256];
static int kn_work_of[256];

static int kn_poll(struct napi_struct *n, int budget)
{
	struct kt_napi *k = (struct kt_napi *)n;
	int work = k->left < budget ? k->left : budget;
	struct napi_stats st;

	napi_get_stats(&st);
	if (kn_norder < 256) {
		kn_order[kn_norder] = (int)(k - kn);
		kn_run_of[kn_norder] = st.rx_action_runs;
		kn_work_of[kn_norder] = work;
		kn_norder++;
	}
	k->polls++;
	if (budget > k->max_budget_seen)
		k->max_budget_seen = budget;
	k->left -= work;
	if (work < budget && napi_complete_done(n, work))
		k->done++;
	return work;
}

int kt_napi_budget(void)
{
	struct napi_stats s0, s1;
	int frames[KN] = { 500, 100, 10, 300, 300, 300, 300 };
	uint64_t run_sum = 0, cur_run = 0;

	napi_get_stats(&s0);
	kn_norder = 0;
	for (int i = 0; i < KN; i++) {
		mm_memset(&kn[i], 0, sizeof(kn[i]));
		netif_napi_add(NULL, &kn[i].napi, kn_poll, NAPI_POLL_WEIGHT);
		KT(!napi_schedule(&kn[i].napi), "a napi that is not enabled");
		napi_enable(&kn[i].napi);
		kn[i].left = frames[i];
	}
	for (int i = 0; i < KN; i++)
		KT(napi_schedule(&kn[i].napi) == 1, "schedule");
	KT(napi_schedule(&kn[0].napi) == 0, "scheduled twice");
	softirq_drain();
	napi_get_stats(&s1);
	for (int i = 0; i < KN; i++) {
		KT(kn[i].left == 0 && kn[i].done == 1, "napi %d: left %d done %d",
		   i, kn[i].left, kn[i].done);
		KT(kn[i].max_budget_seen == NAPI_POLL_WEIGHT, "weight");
	}
	/* first run: 0..5 (64+64+10+64+64+64 > 300: the budget is used
	 * up); the next run starts with the one not polled yet (6), then
	 * those that used their whole weight, in order */
	KT(kn_order[0] == 0 && kn_order[1] == 1 && kn_order[2] == 2 &&
		   kn_order[3] == 3 && kn_order[4] == 4 && kn_order[5] == 5 &&
		   kn_order[6] == 6 && kn_order[7] == 0 && kn_order[8] == 1,
	   "poll order %d %d %d %d %d %d %d %d %d", kn_order[0], kn_order[1],
	   kn_order[2], kn_order[3], kn_order[4], kn_order[5], kn_order[6],
	   kn_order[7], kn_order[8]);
	KT(kn_run_of[6] != kn_run_of[5], "the budget did not end the run");
	// no run did much more than the budget
	cur_run = kn_run_of[0];
	for (int i = 0; i <= kn_norder; i++) {
		if (i == kn_norder || kn_run_of[i] != cur_run) {
			KT(run_sum < NETDEV_BUDGET + NAPI_POLL_WEIGHT,
			   "a run handled %llu frames",
			   (unsigned long long)run_sum);
			if (i == kn_norder)
				break;
			cur_run = kn_run_of[i];
			run_sum = 0;
		}
		run_sum += (uint64_t)kn_work_of[i];
	}
	KT(s1.time_squeeze > s0.time_squeeze, "no time squeeze");
	// an idle napi is scheduled again and polled again
	kn[1].left = 5;
	KT(napi_schedule(&kn[1].napi) == 1, "schedule again");
	kn[1].done = 0;
	softirq_drain();
	KT(kn[1].left == 0 && kn[1].done == 1, "second round");
	napi_disable(&kn[1].napi);
	kn[1].left = 5;
	KT(napi_schedule(&kn[1].napi) == 0, "a disabled napi was scheduled");
	softirq_drain();
	KT(kn[1].left == 5, "a disabled napi was polled");
	napi_enable(&kn[1].napi);
	KT(napi_schedule(&kn[1].napi) == 1, "re-enabled");
	softirq_drain();
	KT(kn[1].left == 0, "polled after enable");
	return 0;
}

/* GRO: a burst of in-order segments of one flow is merged into one; a gap
 * or a flag ends the run; everything reaches the protocol once, in order
 * (here: counted by the merge statistics). */
static uint8_t gro_frames[16][1600];
static unsigned gro_len[16];
static int gro_nframes, gro_next;
static net_device_t gro_dev;

static void kt_put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void kt_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static unsigned kt_tcp_frame(uint8_t *f, uint32_t seq, uint8_t flags,
			     unsigned plen)
{
	uint8_t *ip = f + 14, *tcp = ip + 20;
	uint16_t c;
	uint32_t s;

	mm_memset(f, 0, 14 + 40 + plen);
	for (int i = 0; i < 6; i++)
		f[i] = gro_dev.mac_addr[i];
	f[6] = 0x02;
	f[11] = 0x09;
	kt_put16(f + 12, ETH_P_IP);
	ip[0] = 0x45;
	kt_put16(ip + 2, 40 + plen);
	kt_put16(ip + 6, 0x4000);
	ip[8] = 64;
	ip[9] = 6;
	kt_put32(ip + 12, IP4_ADDR(10, 0, 0, 9));
	kt_put32(ip + 16, IP4_ADDR(10, 0, 0, 1));
	c = ipv4_checksum(ip, 20);
	mm_memcpy(ip + 10, &c, 2);
	kt_put16(tcp, 5000);
	kt_put16(tcp + 2, 6000);
	kt_put32(tcp + 4, seq);
	kt_put32(tcp + 8, 777);
	tcp[12] = 5 << 4;
	tcp[13] = flags;
	kt_put16(tcp + 14, 1000);
	for (unsigned i = 0; i < plen; i++)
		tcp[20 + i] = (uint8_t)(seq + i);
	s = csum_tcpudp_nofold(IP4_ADDR(10, 0, 0, 9), IP4_ADDR(10, 0, 0, 1),
			       (uint16_t)(20 + plen), 6,
			       csum_partial(tcp, 20 + plen, 0));
	c = csum_fold(s);
	mm_memcpy(tcp + 16, &c, 2);
	return 14 + 40 + plen;
}

static void gro_add(uint32_t seq, uint8_t flags, unsigned plen)
{
	gro_len[gro_nframes] = kt_tcp_frame(gro_frames[gro_nframes], seq,
					    flags, plen);
	gro_nframes++;
}

static int gro_poll(struct napi_struct *n, int budget)
{
	int work = 0;

	while (work < budget && gro_next < gro_nframes) {
		sk_buff_t *skb = skb_alloc(gro_len[gro_next]);

		if (!skb)
			break;
		mm_memcpy(skb_append(skb, gro_len[gro_next]),
			  gro_frames[gro_next], gro_len[gro_next]);
		skb->dev = &gro_dev;
		napi_gro_receive(n, skb);
		gro_next++;
		work++;
	}
	if (work < budget)
		napi_complete_done(n, work);
	return work;
}

int kt_gro(void)
{
	static struct napi_struct gn;
	struct napi_stats s0, s1;
	net_device_t *d0 = net_get_default_device();
	uint32_t seq = 1000;

	mm_memset(&gro_dev, 0, sizeof(gro_dev));
	gro_dev.name = "gro0";
	for (int i = 0; i < 6; i++)
		gro_dev.mac_addr[i] = d0->mac_addr[i];
	gro_dev.mtu = 1500;
	gro_dev.ip_addr = d0->ip_addr;
	gro_dev.netmask = d0->netmask;
	gro_dev.features = NETIF_F_GRO;
	dev_init_core(&gro_dev);
	gro_nframes = 0;
	// 6 full segments, then a gap, 2 more, a PSH, a FIN
	for (int i = 0; i < 6; i++, seq += 1000)
		gro_add(seq, TCP_ACK, 1000);
	seq += 1000; // the gap
	for (int i = 0; i < 2; i++, seq += 1000)
		gro_add(seq, TCP_ACK, 1000);
	gro_add(seq, TCP_ACK | TCP_PSH, 500);
	seq += 500;
	gro_add(seq, TCP_ACK | TCP_FIN, 0);
	// a corrupt segment is never merged
	gro_add(seq + 1, TCP_ACK, 1000);
	gro_frames[gro_nframes - 1][60] ^= 1;

	napi_get_stats(&s0);
	int gro_was = net_gro_enabled;
	net_gro_enabled = 1;
	gro_next = 0;
	netif_napi_add(&gro_dev, &gn, gro_poll, NAPI_POLL_WEIGHT);
	napi_enable(&gn);
	napi_schedule(&gn);
	softirq_drain();
	net_gro_enabled = gro_was;
	napi_get_stats(&s1);
	KT(gro_next == gro_nframes, "fed %d of %d", gro_next, gro_nframes);
	/* merged: 5 into the first, 1 + the PSH one into the second run */
	KT(s1.gro_merged - s0.gro_merged == 7, "merged %llu",
	   (unsigned long long)(s1.gro_merged - s0.gro_merged));
	KT(s1.gro_flushed - s0.gro_flushed == 9, "flushed %llu",
	   (unsigned long long)(s1.gro_flushed - s0.gro_flushed));
	KT(gn.gro_count == 0, "segments left held");
	return 0;
}

/* ------------------------------------------------------------------ */
/* transmit queue                                                       */
/* ------------------------------------------------------------------ */

static net_device_t kd;
static int kd_refuse; // refuse this many frames (-1: always)
static int kd_sent, kd_calls;
static int kd_feed; // each frame taken queues this many more (other CPUs)
static uint8_t kd_seq[512];
static int kd_xmit(uint8_t seq);

static int kd_send(net_device_t *dev, const uint8_t *data, uint16_t len)
{
	(void)dev;
	(void)len;
	kd_calls++;
	if (kd_feed > 0) {
		/* Another sender queues a frame while this one hands its
		 * frames to the driver (the queue is running: it only
		 * queues). */
		kd_feed--;
		(void)kd_xmit(0x77);
	}
	if (kd_refuse) {
		if (kd_refuse > 0)
			kd_refuse--;
		return -1;
	}
	if (kd_sent < 512)
		kd_seq[kd_sent] = data[14];
	kd_sent++;
	return 0;
}

static int kd_xmit(uint8_t seq)
{
	sk_buff_t *skb = skb_alloc(60);

	if (!skb)
		return -2;
	mm_memset(skb_append(skb, 60), 0, 60);
	skb->data[14] = seq;
	skb->dev = &kd;
	return dev_queue_xmit(skb);
}

int kt_xmit_setup(void)
{
	mm_memset(&kd, 0, sizeof(kd));
	kd.name = "kd0";
	kd.mtu = 1500;
	kd.send = kd_send;
	kd.mac_addr[5] = 0x42;
	spinlock_init(&kd.lock, "kd");
	KT(net_register(&kd) == 0, "register");
	KT(kd.core_ready && (kd.flags & IFF_UP) && netif_carrier_ok(&kd),
	   "core state");
	kd_refuse = 0;
	kd_sent = 0;
	kd_calls = 0;
	return 0;
}

int kt_xmit_paths(void)
{
	uint64_t f;
	uint64_t drop0;

	// interrupts on: straight to the driver
	KT(kd_xmit(1) == NET_XMIT_SUCCESS && kd_sent == 1, "direct");
	// interrupts off (a protocol lock): queued, sent at the flush
	f = local_irq_save();
	KT(kd_xmit(2) == NET_XMIT_SUCCESS && kd_xmit(3) == NET_XMIT_SUCCESS,
	   "queued");
	KT(kd_sent == 1 && skb_queue_len(&kd.txq.q) == 2, "not sent yet");
	local_irq_restore(f);
	net_tx_flush_local();
	KT(kd_sent == 3 && kd_seq[1] == 2 && kd_seq[2] == 3, "flushed in order");
	// queue limit
	kd.tx_queue_len = 4;
	drop0 = kd.tx_dropped;
	f = local_irq_save();
	for (int i = 0; i < 6; i++)
		kd_xmit((uint8_t)(10 + i));
	local_irq_restore(f);
	KT(kd.tx_dropped == drop0 + 2, "queue limit: dropped %llu",
	   (unsigned long long)(kd.tx_dropped - drop0));
	softirq_drain(); // NET_TX
	KT(kd_sent == 7 && kd_seq[6] == 13, "the NET_TX softirq sent them");
	kd.tx_queue_len = 1000;
	// a refusing driver: requeued, retried, order kept
	kd_refuse = 3;
	kd_xmit(20);
	kd_xmit(21);
	softirq_drain();
	KT(kd_sent == 9 && kd_seq[7] == 20 && kd_seq[8] == 21,
	   "after refusals: sent %d, %u %u", kd_sent, kd_seq[7], kd_seq[8]);
	KT(kd.txq.requeues >= 3, "requeues %llu",
	   (unsigned long long)kd.txq.requeues);
	// carrier off: dropped
	netif_carrier_off(&kd);
	KT(kd_xmit(30) == NET_XMIT_DROP && kd_sent == 9, "no carrier");
	netif_carrier_on(&kd);
	KT(kd_xmit(31) == NET_XMIT_SUCCESS && kd_sent == 10, "carrier back");
	return 0;
}

// A driver that refuses everything: the queue is dropped after 100 ms.
int kt_xmit_stuck_begin(void)
{
	kd_refuse = -1;
	for (int i = 0; i < 20; i++)
		kd_xmit((uint8_t)(40 + i));
	KT(skb_queue_len(&kd.txq.q) == 20, "queued %u",
	   skb_queue_len(&kd.txq.q));
	return 0;
}

int kt_xmit_stuck_check(void)
{
	KT(skb_queue_len(&kd.txq.q) == 0, "the queue of a stuck device: %u",
	   skb_queue_len(&kd.txq.q));
	KT(kd.txq.state & TXQ_STUCK, "not marked stuck");
	kd_refuse = 0;
	KT(kd_xmit(60) == NET_XMIT_SUCCESS && !(kd.txq.state & TXQ_STUCK) &&
		   kd_seq[kd_sent - 1] == 60,
	   "a frame taken again ends it");
	return 0;
}

int kt_xmit_calls(void)
{
	return kd_calls;
}

/* ------------------------------------------------------------------ */
/* neighbours                                                           */
/* ------------------------------------------------------------------ */

/* NUD state of (ip, the default device), -1 when there is no entry;
 * mac_out (may be NULL) gets its address. */
int kt_neigh_state(unsigned ip, unsigned char *mac_out)
{
	neighbour_t *n = neigh_lookup(&arp_tbl, ip, net_get_default_device());
	int st;

	if (!n)
		return -1;
	st = n->nud_state;
	if (mac_out)
		mm_memcpy(mac_out, n->ha, 6);
	neigh_release(n);
	return st;
}

void kt_neigh_confirm(unsigned ip)
{
	ip_confirm_daddr(ip);
}

unsigned kt_neigh_entries(void)
{
	return arp_tbl.entries;
}

unsigned long long kt_arp_conflicts(void)
{
	return arp_get_conflicts();
}

unsigned long long kt_neigh_stat(int which)
{
	switch (which) {
	case 0:
		return arp_tbl.stats.unres_discards;
	case 1:
		return arp_tbl.stats.forced_gc_runs;
	case 2:
		return arp_tbl.stats.table_fulls;
	case 3:
		return arp_tbl.stats.res_failed;
	default:
		return 0;
	}
}

/* `count' STALE entries 10.1.x.y, as if learned; `hold' keeps a
 * reference on each (an entry in use is never collected). */
static neighbour_t *kt_held[1100];
static int kt_nheld;

int kt_neigh_fill(int count, int hold, int base)
{
	net_device_t *dev = net_get_default_device();
	uint8_t mac[6] = { 0x02, 0, 0, 0x55, 0, 0 };
	int made = 0;

	for (int i = 0; i < count; i++) {
		uint32_t ip = IP4_ADDR(10, 1, 0, 0) + (uint32_t)(base + i);
		neighbour_t *n = __neigh_lookup(&arp_tbl, ip, dev, 1);

		if (!n)
			break;
		mac[4] = (uint8_t)(i >> 8);
		mac[5] = (uint8_t)i;
		neigh_update(n, mac, NUD_STALE, NEIGH_UPDATE_F_OVERRIDE);
		made++;
		if (hold && kt_nheld < 1100)
			kt_held[kt_nheld++] = n;
		else
			neigh_release(n);
	}
	return made;
}

void kt_neigh_unhold(void)
{
	for (int i = 0; i < kt_nheld; i++)
		neigh_release(kt_held[i]);
	kt_nheld = 0;
}

void kt_neigh_gc_now(void)
{
	neigh_periodic_gc(&arp_tbl);
}

/* GRO on the instance's NIC (and the global switch), for an interop run. */
void kt_gro_enable(int on)
{
	net_device_t *dev = net_get_default_device();

	net_gro_enabled = on;
	if (on)
		dev->features |= NETIF_F_GRO;
	else
		dev->features &= ~NETIF_F_GRO;
}

/* A burst of frames from the wire handed over before the receive softirq
 * runs (what a NIC interrupt with several frames does). */
void kt_rx_batch(const unsigned char *buf, const unsigned *lens, int n)
{
	net_device_t *dev = net_get_default_device();
	unsigned off = 0;

	for (int i = 0; i < n; i++) {
		net_rx_packet(dev, buf + off, (uint16_t)lens[i]);
		off += lens[i];
	}
	softirq_drain();
}

unsigned long long kt_gro_merged(void)
{
	struct napi_stats st;

	napi_get_stats(&st);
	return st.gro_merged;
}

/* ------------------------------------------------------------------ */
/* review fixes: transmit bound, interface events, oif routing,        */
/* exception scope, timeout arithmetic                                 */
/* ------------------------------------------------------------------ */

/* One process-context send runs the queue for at most one quota, also
 * while other senders keep it filled; the rest goes to NET_TX. */
int kt_xmit_bounded(void)
{
	int c0 = kd_calls;

	kd_refuse = 0;
	kd_feed = 1000;
	KT(kd_xmit(1) == NET_XMIT_SUCCESS, "xmit");
	KT(kd_calls - c0 <= NETDEV_TX_WEIGHT,
	   "one send ran the driver %d times", kd_calls - c0);
	KT(skb_queue_len(&kd.txq.q) > 0 && (kd.txq.state & TXQ_SCHED),
	   "the rest was not handed to NET_TX (queue %u, state %x)",
	   skb_queue_len(&kd.txq.q), kd.txq.state);
	kd_feed = 0;
	softirq_drain();
	KT(skb_queue_len(&kd.txq.q) == 0, "NET_TX left %u frames",
	   skb_queue_len(&kd.txq.q));
	return 0;
}

static int kt_ifreq(unsigned long req, const char *name, struct ifreq *r)
{
	int i;

	for (i = 0; name[i] && i < IFNAMSIZ - 1; i++)
		r->ifr_name[i] = name[i];
	r->ifr_name[i] = 0;
	return net_ioctl(req, r);
}

static int kt_set_inaddr(unsigned long req, uint32_t a)
{
	struct ifreq r;
	struct sockaddr_in *sin = (struct sockaddr_in *)&r.ifr_addr;

	mm_memset(&r, 0, sizeof(r));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = net_htonl(a);
	return kt_ifreq(req, "eth0", &r);
}

static int kt_learned(uint32_t ip)
{
	net_device_t *dev = net_get_default_device();
	uint8_t mac[6] = { 0x02, 0, 0, 0x66, 0, 1 };
	neighbour_t *n = __neigh_lookup(&arp_tbl, ip, dev, 1);

	if (!n)
		return -1;
	neigh_update(n, mac, NUD_STALE, NEIGH_UPDATE_F_OVERRIDE);
	neigh_release(n);
	return 0;
}

static int kt_known(uint32_t ip)
{
	neighbour_t *n = neigh_lookup(&arp_tbl, ip, net_get_default_device());

	if (!n)
		return 0;
	neigh_release(n);
	return 1;
}

/* Address, netmask, MTU and administrative changes raise their events:
 * the neighbours learned on the device are forgotten, the routes handed
 * out are looked up again. */
int kt_if_events(void)
{
	net_device_t *dev = net_get_default_device();
	uint32_t ip0 = dev->ip_addr, mask0 = dev->netmask, g;
	uint16_t mtu0 = dev->mtu;
	uint32_t n1 = IP4_ADDR(10, 0, 0, 51);
	struct ifreq r;
	uint64_t drop0;
	sk_buff_t *skb;

	// the same address again: nothing happens
	KT(kt_learned(n1) == 0 && kt_known(n1), "learn");
	g = fib_genid();
	KT(kt_set_inaddr(SIOCSIFADDR, ip0) == 0, "SIOCSIFADDR");
	KT(kt_known(n1) && fib_genid() == g, "an unchanged address flushed");
	// another address
	KT(kt_set_inaddr(SIOCSIFADDR, IP4_ADDR(10, 0, 0, 201)) == 0,
	   "SIOCSIFADDR");
	KT(!kt_known(n1), "a neighbour survived the address change");
	KT(fib_genid() != g, "routes not invalidated by the address change");
	KT(kt_set_inaddr(SIOCSIFADDR, ip0) == 0 && dev->ip_addr == ip0,
	   "restore");
	// netmask
	KT(kt_learned(n1) == 0, "learn");
	KT(kt_set_inaddr(SIOCSIFNETMASK, 0xFFFF0000u) == 0, "SIOCSIFNETMASK");
	KT(!kt_known(n1), "a neighbour survived the netmask change");
	KT(kt_set_inaddr(SIOCSIFNETMASK, mask0) == 0, "restore");
	// MTU: routes looked up again (the neighbours stay)
	KT(kt_learned(n1) == 0, "learn");
	g = fib_genid();
	mm_memset(&r, 0, sizeof(r));
	r.ifr_mtu = 1400;
	KT(kt_ifreq(SIOCSIFMTU, "eth0", &r) == 0 && dev->mtu == 1400,
	   "SIOCSIFMTU");
	KT(fib_genid() != g && kt_known(n1), "MTU change: genid %u -> %u",
	   g, fib_genid());
	r.ifr_mtu = mtu0;
	KT(kt_ifreq(SIOCSIFMTU, "eth0", &r) == 0, "restore");
	// down: neighbours gone, nothing sent or received, reported down
	g = fib_genid();
	mm_memset(&r, 0, sizeof(r));
	r.ifr_flags = 0;
	KT(kt_ifreq(SIOCSIFFLAGS, "eth0", &r) == 0, "SIOCSIFFLAGS down");
	KT(!(dev->flags & IFF_UP) && !kt_known(n1) && fib_genid() != g,
	   "down: flags %x, neighbour %d", dev->flags, kt_known(n1));
	mm_memset(&r, 0, sizeof(r));
	KT(kt_ifreq(SIOCGIFFLAGS, "eth0", &r) == 0 && !(r.ifr_flags & IFF_UP),
	   "SIOCGIFFLAGS says up: %x", r.ifr_flags);
	drop0 = dev->tx_dropped;
	skb = skb_alloc(60);
	KT(skb != NULL, "skb");
	mm_memset(skb_append(skb, 60), 0, 60);
	skb->dev = dev;
	KT(dev_queue_xmit(skb) == NET_XMIT_DROP && dev->tx_dropped == drop0 + 1,
	   "a down device sent");
	// up again
	mm_memset(&r, 0, sizeof(r));
	r.ifr_flags = IFF_UP;
	KT(kt_ifreq(SIOCSIFFLAGS, "eth0", &r) == 0 && (dev->flags & IFF_UP),
	   "SIOCSIFFLAGS up");
	mm_memset(&r, 0, sizeof(r));
	KT(kt_ifreq(SIOCGIFFLAGS, "eth0", &r) == 0 &&
		   (r.ifr_flags & (IFF_UP | IFF_RUNNING)) ==
			   (IFF_UP | IFF_RUNNING),
	   "SIOCGIFFLAGS after up: %x", r.ifr_flags);
	// carrier loss (netif_carrier_off): NETDEV_CHANGE via linkwatch
	KT(kt_learned(n1) == 0, "learn");
	netif_carrier_off(dev);
	linkwatch_run();
	KT(!kt_known(n1), "a neighbour survived the carrier loss");
	netif_carrier_on(dev);
	linkwatch_run();
	return 0;
}

/* Output routes honour the requested device. */
int kt_route_oif(void)
{
	net_device_t *eth = net_get_default_device();
	struct ip_route rt;
	uint32_t x = IP4_ADDR(10, 0, 0, 9);

	kd.ip_addr = IP4_ADDR(192, 168, 77, 1);
	kd.netmask = 0xFFFFFF00u;
	KT(route_add(IP4_ADDR(10, 0, 0, 0), 0xFFFF0000u, 0, &kd, 0, RTF_UP) ==
		   0,
	   "route via kd0");
	KT(route_add(IP4_ADDR(172, 20, 0, 0), 0xFFFF0000u, 0, eth, 0,
		     RTF_UP) == 0,
	   "route via eth0");
	// no device asked: the longest prefix (eth0's /24)
	KT(ip_route_out(x, 0, NULL, &rt) == 0 && rt.dev == eth &&
		   rt.saddr == eth->ip_addr,
	   "unconstrained: %s", rt.dev ? rt.dev->name : "-");
	ip_route_put(&rt);
	// bound to kd0: its own /16, its own source address
	KT(ip_route_out(x, 0, &kd, &rt) == 0 && rt.dev == &kd &&
		   rt.saddr == kd.ip_addr && rt.dst,
	   "oif kd0: %s saddr %08x", rt.dev ? rt.dev->name : "-", rt.saddr);
	ip_route_put(&rt);
	KT(ip_route_out(x, 0, eth, &rt) == 0 && rt.dev == eth, "oif eth0");
	ip_route_put(&rt);
	// kd0 has no route there: out of kd0 all the same, never eth0
	KT(ip_route_out(IP4_ADDR(172, 20, 1, 1), 0, &kd, &rt) == 0 &&
		   rt.dev == &kd,
	   "oif kd0 without a kd0 route: %s", rt.dev ? rt.dev->name : "-");
	ip_route_put(&rt);
	// an exception made on eth0's path is neither used nor dropped
	pmtu_set(x, 1000);
	KT(ip_route_out(x, 0, &kd, &rt) == 0 && rt.mtu == kd.mtu,
	   "kd0 path took eth0's exception: mtu %u", rt.mtu);
	ip_route_put(&rt);
	KT(pmtu_get(x) == 1000, "a kd0 lookup removed eth0's exception");
	KT(ip_route_out(x, 0, NULL, &rt) == 0 && rt.mtu == 1000,
	   "eth0 path mtu %u", rt.mtu);
	ip_route_put(&rt);
	fib_exceptions_flush();
	KT(route_del(IP4_ADDR(10, 0, 0, 0), 0xFFFF0000u, 0) == 0 &&
		   route_del(IP4_ADDR(172, 20, 0, 0), 0xFFFF0000u, 0) == 0,
	   "route del");
	return 0;
}

/* A path-MTU exception invalidates the cached routes of its destination
 * only: no FIB generation change, other destinations keep theirs. */
int kt_exc_scope(void)
{
	uint32_t x = IP4_ADDR(10, 0, 0, 78), y = IP4_ADDR(10, 0, 0, 77);
	struct dst_cache cx = { 0 }, cy = { 0 };
	struct dst_entry *d = NULL, *c;
	uint32_t g;

	KT(ip_route_output(y, 0, NULL, &d) == 0, "route");
	dst_cache_set(&cx, d, x);
	dst_cache_set(&cy, d, y);
	dst_release(d);
	g = fib_genid();
	pmtu_set(x, 1000);
	KT(pmtu_get(x) == 1000, "exception");
	KT(fib_genid() == g, "an exception changed the FIB generation");
	c = dst_cache_get(&cy);
	KT(c != NULL, "another destination's cached route was dropped");
	dst_release(c);
	KT(dst_cache_get(&cx) == NULL,
	   "the destination's cached route survived its exception");
	KT(ip_route_output(x, 0, NULL, &d) == 0 && (d->flags & DST_HOST) &&
		   dst_mtu(d) == 1000,
	   "the exception's entry");
	dst_cache_set(&cx, d, x);
	dst_release(d);
	c = dst_cache_get(&cx);
	KT(c && (c->flags & DST_HOST), "the exception's entry is cached");
	dst_release(c);
	// lowered again: the exception's own entry is replaced
	pmtu_set(x, 900);
	KT(dst_cache_get(&cx) == NULL, "a lowered exception left the old entry");
	c = dst_cache_get(&cy);
	KT(c != NULL, "another destination's cached route was dropped");
	dst_release(c);
	dst_cache_reset(&cx);
	dst_cache_reset(&cy);
	fib_exceptions_flush();
	return 0;
}

/* recvmmsg's timeout: deadline and time left, no overflow. */
int kt_ts_math(void)
{
	uint64_t now = 1000000000ULL * 5, dl = 0;
	int64_t left[2];

	KT(net_ts_deadline(now, 2, 500, &dl) == 0 &&
		   dl == now + 2000000500ULL,
	   "plain deadline");
	net_ts_left(now + 1000000000ULL, now, dl, 0, 2, 500, left);
	KT(left[0] == 1 && left[1] == 500, "left %lld.%09lld",
	   (long long)left[0], (long long)left[1]);
	net_ts_left(dl + 7, now, dl, 0, 2, 500, left);
	KT(left[0] == 0 && left[1] == 0, "past the deadline");
	// "forever": INT64_MAX seconds does not wrap into the past
	KT(net_ts_deadline(now, 0x7FFFFFFFFFFFFFFFLL, 999999999, &dl) == 1,
	   "INT64_MAX s did not saturate");
	net_ts_left(now + 1500000000ULL, now, 0, 1, 0x7FFFFFFFFFFFFFFFLL, 0,
		    left);
	KT(left[0] == 0x7FFFFFFFFFFFFFFFLL - 2 && left[1] == 500000000,
	   "forever left %lld.%09lld", (long long)left[0],
	   (long long)left[1]);
	// just beyond what 64 bits of ns hold
	KT(net_ts_deadline(now, (int64_t)((~0ULL - now) / 1000000000ULL) + 1,
			   0, &dl) == 1,
	   "edge");
	KT(net_ts_deadline(now, (int64_t)((~0ULL - now) / 1000000000ULL) - 1,
			   0, &dl) == 0 &&
		   dl > now,
	   "largest finite");
	return 0;
}
