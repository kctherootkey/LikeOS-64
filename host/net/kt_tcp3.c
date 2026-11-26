/* Kernel-side probes for the regression tests of the TCP review fixes
 * (host/net/t_tcp2.c calls them as n0_kt3_*): the shape of a published
 * TIME_WAIT sock and child, the time-ordered list of segments in flight
 * and the sequence index of the retransmit queue, the TIME_WAIT reuse
 * policy, the global memory state and the internal-pacing switch.  Every
 * check function returns 0 on success, otherwise the line of the failed
 * check with a message in kt_msg.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/net/tcp.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include "kstubs.h"

extern char kt_msg[256];

#define KT(cond, ...)                                                   \
	do {                                                            \
		if (!(cond)) {                                          \
			ksnprintf(kt_msg, sizeof(kt_msg), __VA_ARGS__); \
			return __LINE__;                                \
		}                                                       \
	} while (0)

/* The TIME_WAIT sock of a 4-tuple (local = this instance): published
 * complete -- hashed, bound, its timer armed, one reference each for the
 * table, the timer and this lookup. */
int kt3_tw_published(unsigned lip, unsigned lport, unsigned rip,
		     unsigned rport)
{
	struct tcp_skc *skc = tcp_lookup_established(lip, (uint16_t)lport, rip,
						     (uint16_t)rport);
	struct tcp_tw_sock *tw;
	int refs, bound, hashed, timer;

	KT(skc, "no sock for the tuple");
	if (skc->skc_kind != TCP_SKC_TW) {
		tcp_skc_put(skc);
		KT(0, "kind %d, not TIME_WAIT", skc->skc_kind);
	}
	tw = (struct tcp_tw_sock *)skc;
	refs = __atomic_load_n(&tw->refcount, __ATOMIC_ACQUIRE);
	bound = tw->skc_bound;
	hashed = tw->skc_hashed;
	timer = net_timer_pending(&tw->timer);
	tcp_skc_put(skc);
	KT(hashed && bound && timer, "hashed %d bound %d timer %d", hashed,
	   bound, timer);
	KT(refs == 3, "%d references (table, timer, lookup expected)", refs);
	return 0;
}

/* A full socket of a 4-tuple: bound while hashed, and referenced by more
 * than the hash (its owner: socket or accept queue) besides this lookup. */
int kt3_full_published(unsigned lip, unsigned lport, unsigned rip,
		       unsigned rport)
{
	struct tcp_skc *skc = tcp_lookup_established(lip, (uint16_t)lport, rip,
						     (uint16_t)rport);
	int refs, bound;

	KT(skc, "no sock for the tuple");
	if (skc->skc_kind != TCP_SKC_FULL) {
		tcp_skc_put(skc);
		KT(0, "kind %d, not a full socket", skc->skc_kind);
	}
	refs = __atomic_load_n(&skc->refcount, __ATOMIC_ACQUIRE);
	bound = skc->skc_bound;
	tcp_skc_put(skc);
	KT(bound, "hashed but not bound");
	KT(refs >= 3, "%d references", refs);
	return 0;
}

/* The time-ordered list and the sequence index of a socket's connection
 * (under its lock): the list is consistent both ways, ordered by send
 * time, holds only segments that are queued and not SACKed; every index
 * entry at or above snd_una names a queued segment that starts there, in
 * ascending order.  *n_list / *n_idx: their lengths. */
int kt3_flight_check(int sock, unsigned *n_list, unsigned *n_idx)
{
	net_socket_t *s = sock_get(sock);
	tcp_conn_t *c = s ? s->tcp : NULL;
	uint64_t f, last_tx = 0;
	sk_buff_t *prev = NULL;
	unsigned n = 0;
	int line = 0;

	*n_list = *n_idx = 0;
	if (!c)
		return 0;
	tcp_lock_acquire(&c->lock, &f);
	for (sk_buff_t *skb = c->tsorted_head; skb; skb = skb->ts_next) {
		const struct tcp_skb_cb *cb = TCP_SKB_CB(skb);

		if (skb->ts_prev != prev) {
			ksnprintf(kt_msg, sizeof(kt_msg),
				  "list entry %u: broken back link", n);
			line = __LINE__;
			break;
		}
		if (cb->sacked & TCPCB_SACKED_ACKED) {
			ksnprintf(kt_msg, sizeof(kt_msg),
				  "list entry %u is SACKed", n);
			line = __LINE__;
			break;
		}
		if (cb->tx_us < last_tx) {
			ksnprintf(kt_msg, sizeof(kt_msg),
				  "list entry %u sent before the one ahead", n);
			line = __LINE__;
			break;
		}
		/* in the sequence space of the queues (a cheap stand-in for
		 * membership: an skb no queue holds would be freed) */
		if (before(cb->end_seq, c->snd_una) ||
		    after(cb->seq, c->write_seq)) {
			ksnprintf(kt_msg, sizeof(kt_msg),
				  "list entry %u (%u..%u) is outside %u..%u", n,
				  cb->seq, cb->end_seq, c->snd_una,
				  c->write_seq);
			line = __LINE__;
			break;
		}
		last_tx = cb->tx_us;
		prev = skb;
		n++;
	}
	if (!line && c->tsorted_tail != prev) {
		ksnprintf(kt_msg, sizeof(kt_msg), "list tail is wrong");
		line = __LINE__;
	}
	*n_list = n;
	for (uint32_t i = 0; !line && i < c->rtx_idx_n; i++) {
		uint32_t seq = c->rtx_idx[i].seq;

		if (i && !after(seq, c->rtx_idx[i - 1].seq)) {
			ksnprintf(kt_msg, sizeof(kt_msg),
				  "index entry %u out of order", i);
			line = __LINE__;
			break;
		}
		if (before(seq, c->snd_una))
			continue; // stale: never followed
		if (TCP_SKB_CB(c->rtx_idx[i].skb)->seq != seq) {
			ksnprintf(kt_msg, sizeof(kt_msg),
				  "index entry %u (seq %u) names no segment", i,
				  seq);
			line = __LINE__;
			break;
		}
	}
	*n_idx = c->rtx_idx_n;
	tcp_lock_release(&c->lock, f);
	return line;
}

/* tcp_twsk_unique() on hand-made TIME_WAIT socks: timestamps required,
 * a bound port reuses at once, an ephemeral one only on loopback after
 * the reuse delay and never from FIN_WAIT2; the new connection starts
 * beyond the old sequence space with the old timestamp state.  Needs
 * tcp_now() > 3 s. */
int kt3_twsk_unique_policy(void)
{
	static struct tcp_tw_sock tw;
	tcp_conn_t *c = tcp_conn_alloc();
	uint64_t now = tcp_now();
	int line = 0;

	KT(c, "no connection");
	KT(now > tcp_ms(3000), "too early (%llu ticks)",
	   (unsigned long long)now);
#define TW_SET(rip, ts, age_ms, sub)                                     \
	do {                                                             \
		mm_memset(&tw, 0, sizeof(tw));                           \
		tw.skc_kind = TCP_SKC_TW;                                \
		tw.substate = (sub);                                     \
		tw.local_ip = ((rip) >> 24) == 127 ? 0x7F000001u :       \
					       0x0A000001u;              \
		tw.remote_ip = (rip);                                    \
		tw.ts_enabled = (ts);                                    \
		tw.ts_recent = 4242;                                     \
		tw.ts_recent_stamp = (ts) ? now - tcp_ms(10) : 0;        \
		tw.entry_stamp = now - tcp_ms(age_ms);                   \
		tw.snd_nxt = 100000;                                     \
		c->ts_recent = 0;                                        \
		c->ts_recent_stamp = 0;                                  \
	} while (0)
#define EXPECT(user, want, what)                                         \
	do {                                                             \
		int r_ = tcp_twsk_unique(&tw, (user), c);                \
		if (r_ != (want)) {                                      \
			ksnprintf(kt_msg, sizeof(kt_msg),                \
				  "%s: %d, expected %d", (what), r_,     \
				  (want));                               \
			line = __LINE__;                                 \
			goto out;                                        \
		}                                                        \
	} while (0)
	TW_SET(0x7F000001u, 1, 2000, TCP_STATE_TIME_WAIT);
	EXPECT(0, 1, "ephemeral port, loopback, 2 s old");
	if (c->write_seq != 100000u + 65537u || c->ts_recent != 4242 ||
	    !c->ts_recent_stamp) {
		ksnprintf(kt_msg, sizeof(kt_msg),
			  "takeover: write_seq %u ts_recent %u", c->write_seq,
			  c->ts_recent);
		line = __LINE__;
		goto out;
	}
	EXPECT(1, 0, "the same sock again (claimed)");
	TW_SET(0x7F000001u, 1, 500, TCP_STATE_TIME_WAIT);
	EXPECT(0, 0, "ephemeral port, loopback, 0.5 s old");
	TW_SET(0x0A000002u, 1, 5000, TCP_STATE_TIME_WAIT);
	EXPECT(0, 0, "ephemeral port, not loopback");
	TW_SET(0x0A000002u, 1, 0, TCP_STATE_TIME_WAIT);
	EXPECT(1, 1, "bound port, not loopback, at once");
	TW_SET(0x7F000001u, 0, 5000, TCP_STATE_TIME_WAIT);
	EXPECT(1, 0, "bound port, no timestamps");
	TW_SET(0x7F000001u, 0, 5000, TCP_STATE_TIME_WAIT);
	EXPECT(0, 0, "ephemeral port, loopback, no timestamps");
	TW_SET(0x7F000001u, 1, 5000, TCP_STATE_FIN_WAIT_2);
	EXPECT(0, 0, "ephemeral port, FIN_WAIT2");
	TW_SET(0x7F000001u, 1, 5000, TCP_STATE_FIN_WAIT_2);
	EXPECT(1, 1, "bound port, FIN_WAIT2");
out:
	c->state = TCP_STATE_CLOSED;
	c->proto_ref = 0;
	tcp_conn_put(c);
	tcp_reap_pending();
#undef TW_SET
#undef EXPECT
	return line;
}

void kt3_internal_pacing(int on)
{
	tcp_sysctl_internal_pacing = on;
}

/* rmem_alloc and the out-of-order segment count of a socket's
 * connection; -1 without one. */
int kt3_rmem(int sock, unsigned *rmem, unsigned *ooo)
{
	net_socket_t *s = sock_get(sock);
	tcp_conn_t *c = s ? s->tcp : NULL;
	uint64_t f;
	unsigned n = 0;

	*rmem = *ooo = 0;
	if (!c)
		return -1;
	tcp_lock_acquire(&c->lock, &f);
	for (struct rb_node *r = rb_first(&c->ooo_queue); r; r = rb_next(r))
		n++;
	*rmem = c->rmem_alloc;
	*ooo = n;
	tcp_lock_release(&c->lock, f);
	return 0;
}
