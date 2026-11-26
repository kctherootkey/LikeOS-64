// LikeOS TCP over IPv4: hash tables, lookup, the receive entry, initial
// sequence numbers, SYN cookies and the stateless replies.
//
// Tables
// ------
//   established hash  every connection that has a 4-tuple: full sockets
//                     from SYN_SENT on, request socks, TIME_WAIT socks.
//                     TCP_EHASH_SIZE chains, a lock per TCP_EHASH_LOCKS
//                     stripe.  Key: the whole 4-tuple, seeded at boot.
//   listening hash    listeners by local port (TCP_LHASH_SIZE chains, one
//                     lock).
//   bind hash         every connection and TIME_WAIT sock by local port:
//                     the ephemeral-port allocator asks it whether a port
//                     is still in use (TCP_BHASH_SIZE chains, one lock).
// All the locks are leaves (tcp.h).  A lookup takes a reference under the
// bucket lock and returns it to the caller, who drops it when done.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/tcp.h>
#include <kernel/io/console.h>
#include <kernel/io/tty.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/slab.h>
#include <kernel/ke/syscall.h>
#include <kernel/dev/rand/random.h>

#define TCP_EHASH_SIZE 16384
#define TCP_EHASH_LOCKS 256
#define TCP_LHASH_SIZE 32
#define TCP_BHASH_SIZE 4096

static struct tcp_skc *g_ehash[TCP_EHASH_SIZE];
static spinlock_t g_ehash_locks[TCP_EHASH_LOCKS];
static struct tcp_skc *g_lhash[TCP_LHASH_SIZE];
static spinlock_t g_lhash_lock = SPINLOCK_INIT("tcp_lhash");
static struct tcp_skc *g_bhash[TCP_BHASH_SIZE];
static spinlock_t g_bhash_lock = SPINLOCK_INIT("tcp_bhash");
static uint64_t g_hash_seed;

// ISN and timestamp-offset secret (SipHash-2-4), SYN cookie secrets.
static uint8_t tcp_isn_secret[16];
static uint8_t tcp_cookie_secret[2][16];

static inline uint64_t tcp_mix64(uint64_t x)
{
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ULL;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBULL;
	x ^= x >> 31;
	return x;
}

static inline uint32_t tcp_ehashfn(uint32_t laddr, uint16_t lport,
				   uint32_t raddr, uint16_t rport)
{
	uint64_t x = ((uint64_t)raddr << 32 | (uint32_t)rport << 16 | lport) ^
		     g_hash_seed;

	x = tcp_mix64(x ^ ((uint64_t)laddr * 0x9E3779B97F4A7C15ULL));
	return (uint32_t)x & (TCP_EHASH_SIZE - 1);
}

static inline spinlock_t *tcp_ehash_lock(uint32_t h)
{
	return &g_ehash_locks[h & (TCP_EHASH_LOCKS - 1)];
}

static inline uint32_t tcp_lhashfn(uint16_t lport)
{
	return (uint32_t)(lport ^ (lport >> 5)) & (TCP_LHASH_SIZE - 1);
}

static inline uint32_t tcp_bhashfn(uint16_t lport)
{
	return (uint32_t)(lport * 2654435761u >> 20) & (TCP_BHASH_SIZE - 1);
}

static inline int tcp_skc_match(const struct tcp_skc *s, uint32_t laddr,
				uint16_t lport, uint32_t raddr, uint16_t rport)
{
	return s->local_port == lport && s->remote_port == rport &&
	       s->remote_ip == raddr && s->local_ip == laddr;
}

void tcp_hash_init(void)
{
	for (int i = 0; i < TCP_EHASH_LOCKS; i++)
		spinlock_init(&g_ehash_locks[i], "tcp_ehash");
	random_get_bytes(&g_hash_seed, sizeof(g_hash_seed), 0);
	random_get_bytes(tcp_isn_secret, sizeof(tcp_isn_secret), 0);
	random_get_bytes(tcp_cookie_secret, sizeof(tcp_cookie_secret), 0);
}

// ---- bind hash ----
void tcp_bhash_add(struct tcp_skc *skc)
{
	uint32_t h = tcp_bhashfn(skc->local_port);
	uint64_t f;

	spin_lock_irqsave(&g_bhash_lock, &f);
	if (!skc->skc_bound) {
		skc->skc_bind_next = g_bhash[h];
		g_bhash[h] = skc;
		skc->skc_bound = 1;
	}
	spin_unlock_irqrestore(&g_bhash_lock, f);
}

void tcp_bhash_remove(struct tcp_skc *skc)
{
	uint32_t h = tcp_bhashfn(skc->local_port);
	uint64_t f;

	spin_lock_irqsave(&g_bhash_lock, &f);
	if (skc->skc_bound) {
		struct tcp_skc **pp = &g_bhash[h];

		while (*pp && *pp != skc)
			pp = &(*pp)->skc_bind_next;
		if (*pp)
			*pp = skc->skc_bind_next;
		skc->skc_bind_next = NULL;
		skc->skc_bound = 0;
	}
	spin_unlock_irqrestore(&g_bhash_lock, f);
}

int tcp_local_port_in_use(uint16_t port)
{
	uint32_t h = tcp_bhashfn(port);
	uint64_t f;
	int used = 0;

	spin_lock_irqsave(&g_bhash_lock, &f);
	for (struct tcp_skc *s = g_bhash[h]; s; s = s->skc_bind_next) {
		if (s->local_port == port) {
			used = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&g_bhash_lock, f);
	return used;
}

// ---- established hash ----
static void tcp_ehash_link_locked(struct tcp_skc *skc, uint32_t h)
{
	skc->skc_next = g_ehash[h];
	g_ehash[h] = skc;
	skc->skc_hashed = 1;
}

static int tcp_ehash_unlink_locked(struct tcp_skc *skc, uint32_t h)
{
	struct tcp_skc **pp = &g_ehash[h];

	while (*pp && *pp != skc)
		pp = &(*pp)->skc_next;
	if (!*pp)
		return 0;
	*pp = skc->skc_next;
	skc->skc_next = NULL;
	skc->skc_hashed = 0;
	return 1;
}

/* An active open: the 4-tuple goes into the established hash.  A
 * TIME_WAIT sock that holds it is reused when tcp_twsk_unique() allows
 * (the new connection's ISN is then placed beyond the old one's sequence
 * space); any other holder makes the connect fail.
 *
 * The connection is in the bind hash before it is visible in the
 * established hash (a reset or ICMP error that finishes it right away must
 * find it bound, so that finishing unbinds it).  A TIME_WAIT sock is only
 * claimed once the whole chain was looked at: a second holder of the
 * tuple fails the connect without killing anything. */
int tcp_hash_connect(tcp_conn_t *c, int user_port)
{
	uint32_t h = tcp_ehashfn(c->local_ip, c->local_port, c->remote_ip,
				 c->remote_port);
	spinlock_t *lk = tcp_ehash_lock(h);
	struct tcp_tw_sock *tw = NULL;
	uint64_t f;

	tcp_bhash_add((struct tcp_skc *)c);
	spin_lock_irqsave(lk, &f);
	for (struct tcp_skc *s = g_ehash[h]; s; s = s->skc_next) {
		if (!tcp_skc_match(s, c->local_ip, c->local_port, c->remote_ip,
				   c->remote_port))
			continue;
		if (s->skc_kind == TCP_SKC_TW && !tw) {
			tw = (struct tcp_tw_sock *)s;
			continue;
		}
		goto busy;
	}
	if (tw) {
		if (!tcp_twsk_unique(tw, user_port, c))
			goto busy;
		tcp_ehash_unlink_locked((struct tcp_skc *)tw, h);
	}
	tcp_ehash_link_locked((struct tcp_skc *)c, h);
	spin_unlock_irqrestore(lk, f);
	if (tw) {
		TCPX_INC(TCPX_TWREUSED);
		NET_STATS_INC(NET_MIB_TCP_TWREUSED);
		tcp_tw_kill(tw); // drops the table reference
	}
	return 0;
busy:
	spin_unlock_irqrestore(lk, f);
	tcp_bhash_remove((struct tcp_skc *)c);
	return -EADDRINUSE;
}

int tcp_ehash_insert_req(struct tcp_request_sock *req)
{
	uint32_t h = tcp_ehashfn(req->local_ip, req->local_port,
				 req->remote_ip, req->remote_port);
	spinlock_t *lk = tcp_ehash_lock(h);
	uint64_t f;

	spin_lock_irqsave(lk, &f);
	for (struct tcp_skc *s = g_ehash[h]; s; s = s->skc_next) {
		if (tcp_skc_match(s, req->local_ip, req->local_port,
				  req->remote_ip, req->remote_port)) {
			spin_unlock_irqrestore(lk, f);
			return -EEXIST;
		}
	}
	tcp_ehash_link_locked((struct tcp_skc *)req, h);
	spin_unlock_irqrestore(lk, f);
	return 0;
}

void tcp_ehash_remove(struct tcp_skc *skc)
{
	uint32_t h = tcp_ehashfn(skc->local_ip, skc->local_port,
				 skc->remote_ip, skc->remote_port);
	spinlock_t *lk = tcp_ehash_lock(h);
	uint64_t f;

	spin_lock_irqsave(lk, &f);
	if (skc->skc_hashed)
		tcp_ehash_unlink_locked(skc, h);
	spin_unlock_irqrestore(lk, f);
}

int tcp_ehash_replace(struct tcp_skc *old, struct tcp_skc *newsk)
{
	uint32_t h = tcp_ehashfn(old->local_ip, old->local_port,
				 old->remote_ip, old->remote_port);
	spinlock_t *lk = tcp_ehash_lock(h);
	uint64_t f;
	int rc = -1;

	BUG_ON(!tcp_skc_match(newsk, old->local_ip, old->local_port,
			      old->remote_ip, old->remote_port));
	spin_lock_irqsave(lk, &f);
	if (old->skc_hashed && tcp_ehash_unlink_locked(old, h)) {
		tcp_ehash_link_locked(newsk, h);
		rc = 0;
	}
	spin_unlock_irqrestore(lk, f);
	return rc;
}

/* Walk the established hash for full sockets with out-of-order data.  Per
 * chain, references are taken under the bucket lock, a batch at a time,
 * and fn runs on each with no lock held.  The out-of-order test under the
 * bucket lock is a racy hint (the socket's lock is not taken there): fn
 * looks again under the socket's lock.  A long chain is resumed by
 * position: a socket inserted meanwhile may be missed or seen twice --
 * this serves a best-effort reclaim, not an exact census. */
#define TCP_EHASH_WALK_BATCH 32
void tcp_ehash_for_each_ofo(int (*fn)(tcp_conn_t *c, void *arg), void *arg)
{
	tcp_conn_t *batch[TCP_EHASH_WALK_BATCH];

	WARN_ON_ONCE(irqs_disabled());
	for (uint32_t h = 0; h < TCP_EHASH_SIZE; h++) {
		spinlock_t *lk = tcp_ehash_lock(h);
		uint32_t skip = 0;

		while (__atomic_load_n(&g_ehash[h], __ATOMIC_RELAXED)) {
			uint32_t n = 0, pos = 0;
			int stop = 0, more = 0;
			uint64_t f;

			spin_lock_irqsave(lk, &f);
			for (struct tcp_skc *s = g_ehash[h]; s;
			     s = s->skc_next, pos++) {
				tcp_conn_t *c = (tcp_conn_t *)s;

				if (pos < skip)
					continue;
				if (n == TCP_EHASH_WALK_BATCH) {
					more = 1;
					break;
				}
				if (s->skc_kind == TCP_SKC_FULL &&
				    __atomic_load_n(&c->ooo_queue.rb_node,
						    __ATOMIC_RELAXED) &&
				    tcp_skc_tryhold(s))
					batch[n++] = c;
			}
			spin_unlock_irqrestore(lk, f);
			for (uint32_t i = 0; i < n; i++) {
				if (!stop)
					stop = fn(batch[i], arg);
				tcp_conn_put(batch[i]);
			}
			if (stop)
				return;
			if (!more)
				break;
			skip = pos; // strictly grows: the walk ends
		}
	}
}

struct tcp_skc *tcp_lookup_established(uint32_t laddr, uint16_t lport,
				       uint32_t raddr, uint16_t rport)
{
	uint32_t h = tcp_ehashfn(laddr, lport, raddr, rport);
	spinlock_t *lk = tcp_ehash_lock(h);
	struct tcp_skc *found = NULL;
	uint64_t f;

	spin_lock_irqsave(lk, &f);
	for (struct tcp_skc *s = g_ehash[h]; s; s = s->skc_next) {
		if (tcp_skc_match(s, laddr, lport, raddr, rport)) {
			if (tcp_skc_tryhold(s))
				found = s;
			break;
		}
	}
	spin_unlock_irqrestore(lk, f);
	return found;
}

int tcp_4tuple_in_use(uint32_t local_ip, uint16_t local_port,
		      uint32_t remote_ip, uint16_t remote_port)
{
	struct tcp_skc *s;

	if (local_ip) {
		s = tcp_lookup_established(local_ip, local_port, remote_ip,
					   remote_port);
		if (s)
			tcp_skc_put(s);
		return s != NULL;
	}
	/* No local address yet: any connection from this port to the peer
	 * counts (the bind hash has every one of them). */
	{
		uint32_t h = tcp_bhashfn(local_port);
		uint64_t f;
		int used = 0;

		spin_lock_irqsave(&g_bhash_lock, &f);
		for (s = g_bhash[h]; s; s = s->skc_bind_next) {
			if (s->local_port == local_port &&
			    s->remote_port == remote_port &&
			    s->remote_ip == remote_ip) {
				used = 1;
				break;
			}
		}
		spin_unlock_irqrestore(&g_bhash_lock, f);
		return used;
	}
}

// ---- listening hash ----
void tcp_hash_listener(tcp_conn_t *c)
{
	uint32_t h = tcp_lhashfn(c->local_port);
	uint64_t f;

	spin_lock_irqsave(&g_lhash_lock, &f);
	c->skc_next = g_lhash[h];
	g_lhash[h] = (struct tcp_skc *)c;
	c->skc_hashed = 1;
	spin_unlock_irqrestore(&g_lhash_lock, f);
}

void tcp_unhash(tcp_conn_t *c)
{
	uint64_t f;

	if (c->skc_hashed) {
		if (c->state == TCP_STATE_LISTEN) {
			uint32_t h = tcp_lhashfn(c->local_port);
			struct tcp_skc **pp;

			spin_lock_irqsave(&g_lhash_lock, &f);
			pp = &g_lhash[h];
			while (*pp && *pp != (struct tcp_skc *)c)
				pp = &(*pp)->skc_next;
			if (*pp)
				*pp = c->skc_next;
			c->skc_next = NULL;
			c->skc_hashed = 0;
			spin_unlock_irqrestore(&g_lhash_lock, f);
		} else {
			tcp_ehash_remove((struct tcp_skc *)c);
		}
	}
	if (c->skc_bound)
		tcp_bhash_remove((struct tcp_skc *)c);
}

/* The listener for a SYN to laddr:lport: one bound to that address before
 * a wildcard one; among several on the same port (SO_REUSEPORT) the one
 * with the fewest connections waiting, so none of them starves. */
tcp_conn_t *tcp_lookup_listener(uint32_t laddr, uint16_t lport)
{
	uint32_t h = tcp_lhashfn(lport);
	tcp_conn_t *best = NULL;
	int best_exact = 0;
	uint32_t best_load = 0;
	uint64_t f;

	spin_lock_irqsave(&g_lhash_lock, &f);
	for (struct tcp_skc *s = g_lhash[h]; s; s = s->skc_next) {
		tcp_conn_t *l = (tcp_conn_t *)s;
		int exact;
		uint32_t load;

		if (l->local_port != lport || l->state != TCP_STATE_LISTEN)
			continue;
		if (l->local_ip != laddr && l->local_ip != 0)
			continue;
		exact = l->local_ip == laddr;
		load = l->ack_backlog + l->reqsk_qlen;
		if (!best || (exact && !best_exact) ||
		    (exact == best_exact && load < best_load)) {
			best = l;
			best_exact = exact;
			best_load = load;
		}
	}
	if (best && !tcp_skc_tryhold((struct tcp_skc *)best))
		best = NULL;
	spin_unlock_irqrestore(&g_lhash_lock, f);
	return best;
}

// ---------------------------------------------------------------------------
// Initial sequence numbers and timestamp offsets.
// ---------------------------------------------------------------------------
static void tcp_tuple_bytes(uint8_t d[12], uint32_t saddr, uint32_t daddr,
			    uint16_t sport, uint16_t dport)
{
	tcp_put_be32(d, saddr);
	tcp_put_be32(d + 4, daddr);
	tcp_put_be16(d + 8, sport);
	tcp_put_be16(d + 10, dport);
}

/* RFC 6528: ISN = hash(secret, 4-tuple) + a clock advancing ~64K per
 * second, whatever the tick rate. */
uint32_t tcp_isn(uint32_t saddr, uint32_t daddr, uint16_t sport, uint16_t dport)
{
	uint8_t d[12];
	uint64_t hz = timer_get_frequency();

	tcp_tuple_bytes(d, saddr, daddr, sport, dport);
	if (hz == 0)
		hz = 100;
	return (uint32_t)siphash_2_4(tcp_isn_secret, d, 12) +
	       (uint32_t)(timer_ticks() * 65536ULL / hz);
}

/* RFC 7323 appendix A: each 4-tuple gets its own TSval origin, so the
 * timestamps reveal neither the uptime nor a link between connections.
 * Derived from the tuple, so a SYN cookie can recompute it. */
uint32_t tcp_ts_offset_for(uint32_t saddr, uint32_t daddr, uint16_t sport,
			   uint16_t dport)
{
	uint8_t d[12];

	tcp_tuple_bytes(d, saddr, daddr, sport, dport);
	return (uint32_t)(siphash_2_4(tcp_isn_secret, d, 12) ^
			  0x54534F46U /* 'TSOF' */);
}

/* The timestamp clock runs in milliseconds, derived from the tick count
 * (one tick of granularity, no timer read per segment). */
uint32_t tcp_ts_clock_ms(void)
{
	return (uint32_t)((timer_ticks() * timer_us_per_tick()) / 1000ULL);
}

// ---------------------------------------------------------------------------
// SYN cookies (RFC 4987).
//
//   cookie = H1(tuple) + peer ISN + (counter << 24)
//            + ((H2(tuple, counter) + mss index) & 0xFFFFFF)
//
// The counter advances once per TCP_SYNCOOKIE_PERIOD_MS; a cookie is good
// for the counter it was made with and the one after.  The MSS is one of
// four values (2 bits).  With timestamps, the low 6 bits of our TSval carry
// the window scale (4 bits, 15 = none), SACK and ECN, so a connection made
// from a cookie keeps those options.
// ---------------------------------------------------------------------------
#define COOKIEBITS 24
#define COOKIEMASK ((1u << COOKIEBITS) - 1)
#define MAX_SYNCOOKIE_AGE 2
#define TSBITS 6
#define TSMASK ((1u << TSBITS) - 1)
#define TS_OPT_WSCALE_MASK 0xFu
#define TS_OPT_SACK 0x10u
#define TS_OPT_ECN 0x20u

static const uint16_t msstab[] = { 536, 1300, 1440, 1460 };

static uint32_t cookie_counter(void)
{
	return (uint32_t)(timer_ticks() / tcp_ms(TCP_SYNCOOKIE_PERIOD_MS));
}

static uint32_t cookie_hash(uint32_t saddr, uint32_t daddr, uint16_t sport,
			    uint16_t dport, uint32_t count, int c)
{
	uint8_t d[16];

	tcp_put_be32(d, saddr);
	tcp_put_be32(d + 4, daddr);
	tcp_put_be16(d + 8, sport);
	tcp_put_be16(d + 10, dport);
	tcp_put_be32(d + 12, count);
	return (uint32_t)siphash_2_4(tcp_cookie_secret[c], d, 16);
}

uint32_t tcp_cookie_init_sequence(uint32_t saddr, uint32_t daddr,
				  uint16_t sport, uint16_t dport, uint32_t sseq,
				  uint16_t *mssp)
{
	uint32_t count = cookie_counter(), idx;

	for (idx = sizeof(msstab) / sizeof(msstab[0]) - 1; idx > 0; idx--)
		if (*mssp >= msstab[idx])
			break;
	*mssp = msstab[idx];
	return cookie_hash(saddr, daddr, sport, dport, 0, 0) + sseq +
	       (count << COOKIEBITS) +
	       ((cookie_hash(saddr, daddr, sport, dport, count, 1) + idx) &
		COOKIEMASK);
}

uint16_t tcp_cookie_check(uint32_t saddr, uint32_t daddr, uint16_t sport,
			  uint16_t dport, uint32_t sseq, uint32_t cookie)
{
	uint32_t count = cookie_counter(), diff, idx;

	cookie -= cookie_hash(saddr, daddr, sport, dport, 0, 0) + sseq;
	diff = (count - (cookie >> COOKIEBITS)) & (0xFFFFFFFFu >> COOKIEBITS);
	if (diff >= MAX_SYNCOOKIE_AGE)
		return 0;
	idx = (cookie - cookie_hash(saddr, daddr, sport, dport, count - diff,
				    1)) &
	      COOKIEMASK;
	if (idx >= sizeof(msstab) / sizeof(msstab[0]))
		return 0;
	return msstab[idx];
}

uint32_t tcp_cookie_init_timestamp(uint32_t ts_now, int wscale_ok,
				   uint8_t snd_wscale, int sack_ok, int ecn_ok)
{
	uint32_t options = wscale_ok ? (snd_wscale & TS_OPT_WSCALE_MASK) :
				       TS_OPT_WSCALE_MASK;
	uint32_t ts;

	if (sack_ok)
		options |= TS_OPT_SACK;
	if (ecn_ok)
		options |= TS_OPT_ECN;
	ts = (ts_now & ~TSMASK) | options;
	if (after(ts, ts_now))
		ts -= (1u << TSBITS);
	return ts;
}

int tcp_cookie_decode_options(uint32_t tsecr, uint32_t ts_now, int *wscale_ok,
			      uint8_t *snd_wscale, int *sack_ok, int *ecn_ok)
{
	uint32_t options = tsecr & TSMASK;

	/* An echo from the future was not made by us. */
	if (after(tsecr, ts_now))
		return 0;
	*sack_ok = (options & TS_OPT_SACK) != 0;
	*ecn_ok = (options & TS_OPT_ECN) != 0;
	if ((options & TS_OPT_WSCALE_MASK) == TS_OPT_WSCALE_MASK) {
		*wscale_ok = 0;
		*snd_wscale = 0;
	} else {
		*wscale_ok = 1;
		*snd_wscale = (uint8_t)(options & TS_OPT_WSCALE_MASK);
		if (*snd_wscale > TCP_MAX_WSCALE)
			return 0;
	}
	return 1;
}

// ---------------------------------------------------------------------------
// RFC 5961 challenge ACKs: at most TCP_CHALLENGE_ACK_LIMIT per second for
// the whole host, the per-second budget randomized so an attacker cannot
// count it out exactly.
// ---------------------------------------------------------------------------
static spinlock_t g_challenge_lock = SPINLOCK_INIT("tcp_challenge");
static uint64_t g_challenge_second;
static uint32_t g_challenge_count;

int tcp_challenge_ack_allowed(void)
{
	uint64_t f, now = timer_ticks() / tcp_ms(1000);
	int ok = 0;

	spin_lock_irqsave(&g_challenge_lock, &f);
	if (now != g_challenge_second) {
		uint32_t half = (TCP_CHALLENGE_ACK_LIMIT + 1) >> 1;

		g_challenge_second = now;
		g_challenge_count = half + random_u32() % TCP_CHALLENGE_ACK_LIMIT;
	}
	if (g_challenge_count > 0) {
		g_challenge_count--;
		ok = 1;
	}
	spin_unlock_irqrestore(&g_challenge_lock, f);
	return ok;
}

// ---------------------------------------------------------------------------
// Stateless segments.
// ---------------------------------------------------------------------------
static void tcp_v4_send_stateless(uint32_t saddr, uint32_t daddr,
				  uint16_t sport, uint16_t dport, uint32_t seq,
				  uint32_t ack, uint8_t flags, uint16_t win,
				  int ts, uint32_t tsval, uint32_t tsecr)
{
	uint32_t hlen = sizeof(tcp_header_t) + (ts ? 12u : 0u);
	sk_buff_t *skb = skb_alloc_headroom(TCP_MAX_HEADER, 0);
	uint8_t *th;
	uint16_t check;

	if (!skb)
		return;
	th = skb_push(skb, hlen);
	mm_memset(th, 0, hlen);
	tcp_put_be16(th, sport);
	tcp_put_be16(th + 2, dport);
	tcp_put_be32(th + 4, seq);
	tcp_put_be32(th + 8, ack);
	th[12] = (uint8_t)((hlen / 4) << 4);
	th[13] = flags;
	tcp_put_be16(th + 14, win);
	if (ts) {
		th[20] = TCP_OPT_NOP;
		th[21] = TCP_OPT_NOP;
		th[22] = TCP_OPT_TIMESTAMP;
		th[23] = TCP_OPT_TIMESTAMP_LEN;
		tcp_put_be32(th + 24, tsval);
		tcp_put_be32(th + 28, tsecr);
	}
	check = csum_fold(csum_tcpudp_nofold(saddr, daddr, (uint16_t)hlen,
					     IP_PROTO_TCP,
					     csum_partial(th, hlen, 0)));
	mm_memcpy(th + 16, &check, 2);
	skb_reset_transport_header(skb);
	NET_STATS_INC(NET_MIB_TCP_OUTSEGS);
	if (flags & TCP_RST)
		NET_STATS_INC(NET_MIB_TCP_OUTRSTS);
	ip_queue_xmit(skb, saddr, daddr, IP_PROTO_TCP, 0, 0, NULL, NULL);
}

/* A reset in answer to a segment nobody owns (RFC 9293 3.10.7.1): with
 * the segment's ACK as our sequence number when it carries one, else
 * sequence 0 acknowledging everything it occupied. */
void tcp_v4_send_reset(net_device_t *dev, uint32_t saddr, uint32_t daddr,
		       const uint8_t *th, uint32_t seg_len)
{
	uint8_t flags = th[13];
	uint32_t doff = (uint32_t)(th[12] >> 4) * 4;
	uint32_t payload = seg_len > doff ? seg_len - doff : 0;

	(void)dev;
	if (flags & TCP_RST)
		return;
	if (flags & TCP_ACK)
		tcp_v4_send_stateless(saddr, daddr, tcp_get_be16(th + 2),
				      tcp_get_be16(th), tcp_get_be32(th + 8), 0,
				      TCP_RST, 0, 0, 0, 0);
	else
		tcp_v4_send_stateless(
			saddr, daddr, tcp_get_be16(th + 2), tcp_get_be16(th), 0,
			tcp_get_be32(th + 4) + payload +
				((flags & TCP_SYN) ? 1u : 0u) +
				((flags & TCP_FIN) ? 1u : 0u),
			TCP_RST | TCP_ACK, 0, 0, 0, 0);
}

void tcp_v4_send_ack(uint32_t saddr, uint32_t daddr, uint16_t sport,
		     uint16_t dport, uint32_t seq, uint32_t ack, uint32_t win,
		     int ts, uint32_t tsval, uint32_t tsecr)
{
	tcp_v4_send_stateless(saddr, daddr, sport, dport, seq, ack, TCP_ACK,
			      (uint16_t)(win > 0xFFFFu ? 0xFFFFu : win), ts,
			      tsval, tsecr);
}

// ---------------------------------------------------------------------------
// Receive.
// ---------------------------------------------------------------------------
static int tcp_v4_csum_ok(const sk_buff_t *skb, uint32_t saddr, uint32_t daddr)
{
	uint32_t sum = csum_tcpudp_nofold(saddr, daddr, (uint16_t)skb->len,
					  IP_PROTO_TCP,
					  csum_partial(skb->data, skb->len, 0));

	return csum_fold(sum) == 0;
}

/* A segment for a listener with no matching connection: a SYN opens a
 * request sock (or answers with a cookie), an ACK may complete a cookie
 * handshake; everything else with ACK gets a reset. */
static void tcp_v4_listen_rcv(tcp_conn_t *l, sk_buff_t *skb,
			      const tcp_parsed_opts_t *opt, uint32_t saddr,
			      uint32_t daddr)
{
	struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
	uint8_t flags = cb->tcp_flags;
	const uint8_t *th = skb->data;

	if (flags & TCP_RST)
		goto drop;
	if ((flags & (TCP_SYN | TCP_ACK)) == TCP_ACK) {
		int reset = 1;
		tcp_conn_t *child = tcp_cookie_child(l, skb, opt, saddr, daddr,
						     skb->dev, &reset);

		if (child) {
			uint64_t f;

			tcp_lock_acquire(&child->lock, &f);
			tcp_rcv_state_process(child, skb, opt);
			tcp_lock_release(&child->lock, f);
			tcp_conn_put(child);
			return;
		}
		if (reset)
			tcp_v4_send_reset(skb->dev, daddr, saddr, th, skb->len);
		goto drop;
	}
	if (flags & TCP_ACK) { // SYN+ACK to a listener
		tcp_v4_send_reset(skb->dev, daddr, saddr, th, skb->len);
		goto drop;
	}
	if (flags & TCP_SYN) {
		tcp_conn_request(l, skb, opt, saddr, daddr, skb->dev);
		goto drop;
	}
drop:
	skb_put(skb);
}

/* The receive entry: skb->data at the TCP header, IPCB(skb) valid.
 * Consumes the skb.  Softirq context, interrupts on, no lock held. */
int tcp_v4_rcv(sk_buff_t *skb)
{
	struct inet_skb_parm *ip = IPCB(skb);
	uint32_t saddr = ip->saddr, daddr = ip->daddr;
	uint8_t tos = ip->tos; // the ECN field, before cb is reused
	tcp_parsed_opts_t opt;
	struct tcp_skb_cb *cb;
	struct tcp_skc *skc;
	const uint8_t *th;
	uint32_t doff, len = skb->len, plen;
	uint8_t flags;
	uint16_t sport, dport;
	net_device_t *dev = skb->dev;
	uint16_t segs = skb->shinfo ? skb->shinfo->gso_segs : 0;

	/* TCP is unicast: a segment to a broadcast or multicast address is
	 * nobody's (and must never draw a reset). */
	if (ip->rt_type != RTN_LOCAL)
		goto drop;
	if (len < sizeof(tcp_header_t))
		goto bad;
	th = skb->data;
	doff = (uint32_t)(th[12] >> 4) * 4;
	if (doff < sizeof(tcp_header_t) || doff > len)
		goto bad;
	/* The checksum, before anything in the segment is believed. */
	if (skb->ip_summed != CHECKSUM_UNNECESSARY &&
	    !tcp_v4_csum_ok(skb, saddr, daddr)) {
		NET_STATS_INC(NET_MIB_TCP_INCSUMERRORS);
		goto bad;
	}
	flags = th[13]; // with ECE and CWR
	sport = tcp_get_be16(th);
	dport = tcp_get_be16(th + 2);
	// Illegal flag combinations: null scan, SYN+FIN, Xmas.
	if ((flags & 0x3F) == 0 ||
	    (flags & (TCP_SYN | TCP_FIN)) == (TCP_SYN | TCP_FIN) ||
	    (flags & 0x3F) == 0x3F)
		goto drop;
	/* Land attack -- only from the wire: on the loopback device the
	 * same SYN is a socket connecting to itself, a simultaneous open
	 * TCP defines. */
	if ((flags & (TCP_SYN | TCP_ACK)) == TCP_SYN && saddr == daddr &&
	    sport == dport && dev != net_get_loopback())
		goto drop;
	NET_STATS_INC(NET_MIB_TCP_INSEGS);

	tcp_parse_options(th, doff, &opt);
	plen = len - doff;
	cb = TCP_SKB_CB(skb);
	mm_memset(cb, 0, sizeof(*cb));
	cb->seq = tcp_get_be32(th + 4);
	cb->end_seq = cb->seq + plen + ((flags & TCP_SYN) ? 1u : 0u) +
		      ((flags & TCP_FIN) ? 1u : 0u);
	cb->ack_seq = tcp_get_be32(th + 8);
	cb->tcp_flags = flags;
	cb->ip_dsfield = tos;
	cb->window = tcp_get_be16(th + 14);
	cb->urg_ptr = tcp_get_be16(th + 18);
	cb->segs = segs > 1 ? segs : 1;
	cb->tsval = opt.tsval;
	cb->tsecr = opt.tsecr;
	skb_reset_transport_header(skb);
	skb->src_ip = saddr;
	skb->dst_ip = daddr;

	skc = tcp_lookup_established(daddr, dport, saddr, sport);
	if (skc) {
		switch (skc->skc_kind) {
		case TCP_SKC_FULL: {
			tcp_conn_t *c = (tcp_conn_t *)skc;
			uint64_t f;

			tcp_lock_acquire(&c->lock, &f);
			tcp_rcv_state_process(c, skb, &opt);
			tcp_lock_release(&c->lock, f);
			tcp_conn_put(c);
			return 0;
		}
		case TCP_SKC_REQ: {
			tcp_conn_t *child = tcp_check_req(
				(struct tcp_request_sock *)skc, skb, &opt);

			tcp_skc_put(skc);
			if (child) {
				uint64_t f;

				tcp_lock_acquire(&child->lock, &f);
				tcp_rcv_state_process(child, skb, &opt);
				tcp_lock_release(&child->lock, f);
				tcp_conn_put(child);
				return 0;
			}
			goto drop;
		}
		case TCP_SKC_TW: {
			int r = tcp_timewait_state_process(
				(struct tcp_tw_sock *)skc, skb, &opt);

			tcp_skc_put(skc);
			if (r == 1)
				break; // a new SYN: on to the listener
			goto drop;
		}
		default:
			tcp_skc_put(skc);
			goto drop;
		}
	}
	{
		tcp_conn_t *l = tcp_lookup_listener(daddr, dport);

		if (l) {
			tcp_v4_listen_rcv(l, skb, &opt, saddr, daddr);
			tcp_conn_put(l);
			return 0;
		}
	}
	// Nobody: a reset so the peer stops (unless it is one).
	tcp_v4_send_reset(dev, daddr, saddr, skb->data, skb->len);
	goto drop;
bad:
	NET_STATS_INC(NET_MIB_TCP_INERRS);
drop:
	skb_put(skb);
	return 0;
}

// ---------------------------------------------------------------------------
// Option parsing (RFC 9293 3.2, RFC 7323, RFC 2018).
// ---------------------------------------------------------------------------
void tcp_parse_options(const uint8_t *th, uint32_t doff, tcp_parsed_opts_t *out)
{
	const uint8_t *o = th + sizeof(tcp_header_t);
	uint32_t n = doff > sizeof(tcp_header_t) ? doff - sizeof(tcp_header_t) :
						   0;
	uint32_t i = 0;

	mm_memset(out, 0, sizeof(*out));
	out->wscale = -1;
	while (i < n) {
		uint8_t k = o[i], l;

		if (k == TCP_OPT_END)
			break;
		if (k == TCP_OPT_NOP) {
			i++;
			continue;
		}
		if (i + 1 >= n)
			break;
		l = o[i + 1];
		if (l < 2 || i + l > n)
			break;
		switch (k) {
		case TCP_OPT_MSS:
			if (l == TCP_OPT_MSS_LEN)
				out->mss = tcp_get_be16(o + i + 2);
			break;
		case TCP_OPT_WSCALE:
			if (l == TCP_OPT_WSCALE_LEN) {
				uint8_t s = o[i + 2];

				out->wscale = (int8_t)(s > TCP_MAX_WSCALE ?
							       TCP_MAX_WSCALE :
							       s);
			}
			break;
		case TCP_OPT_SACK_PERM:
			if (l == TCP_OPT_SACK_PERM_LEN)
				out->sack_perm = 1;
			break;
		case TCP_OPT_TIMESTAMP:
			if (l == TCP_OPT_TIMESTAMP_LEN) {
				out->ts_present = 1;
				out->tsval = tcp_get_be32(o + i + 2);
				out->tsecr = tcp_get_be32(o + i + 6);
			}
			break;
		case TCP_OPT_SACK: {
			uint32_t blocks = (uint32_t)(l - 2) / 8;

			if ((l - 2) % 8)
				break;
			if (blocks > TCP_MAX_SACK_BLOCKS)
				blocks = TCP_MAX_SACK_BLOCKS;
			for (uint32_t b = 0; b < blocks; b++) {
				out->sack[b].start_seq =
					tcp_get_be32(o + i + 2 + b * 8);
				out->sack[b].end_seq =
					tcp_get_be32(o + i + 6 + b * 8);
			}
			out->sack_count = (uint8_t)blocks;
			break;
		}
		default:
			break;
		}
		i += l;
	}
}

// ---------------------------------------------------------------------------
// Diagnostics: the tables.
// ---------------------------------------------------------------------------
static void tcp_fill_entry(net_tcp_info_t *e, const struct tcp_skc *s)
{
	e->local_ip = s->local_ip;
	e->local_port = s->local_port;
	e->remote_ip = s->remote_ip;
	e->remote_port = s->remote_port;
	e->rx_queue = 0;
	e->tx_queue = 0;
	switch (s->skc_kind) {
	case TCP_SKC_FULL: {
		const tcp_conn_t *c = (const tcp_conn_t *)s;

		e->state = c->state;
		e->rx_queue = c->rcvq_bytes;
		e->tx_queue = c->write_seq - c->snd_una;
		break;
	}
	case TCP_SKC_REQ:
		e->state = TCP_STATE_SYN_RECEIVED;
		break;
	case TCP_SKC_TW:
		e->state = ((const struct tcp_tw_sock *)s)->substate;
		break;
	default:
		e->state = TCP_STATE_CLOSED;
		break;
	}
}

// Snapshot the tables for netstat (SYS_NET_GETINFO).
int net_get_tcp_connections(net_tcp_info_t *entries, int max_entries)
{
	int n = 0;
	uint64_t f;

	spin_lock_irqsave(&g_lhash_lock, &f);
	for (int b = 0; b < TCP_LHASH_SIZE && n < max_entries; b++)
		for (struct tcp_skc *s = g_lhash[b]; s && n < max_entries;
		     s = s->skc_next)
			tcp_fill_entry(&entries[n++], s);
	spin_unlock_irqrestore(&g_lhash_lock, f);
	for (int b = 0; b < TCP_EHASH_SIZE && n < max_entries; b++) {
		spinlock_t *lk = tcp_ehash_lock((uint32_t)b);

		if (!g_ehash[b])
			continue;
		spin_lock_irqsave(lk, &f);
		for (struct tcp_skc *s = g_ehash[b]; s && n < max_entries;
		     s = s->skc_next)
			tcp_fill_entry(&entries[n++], s);
		spin_unlock_irqrestore(lk, f);
	}
	return n;
}

/* Ctrl+N dump: a best-effort walk without locks (diagnostics only; a torn
 * read garbles a line at worst). */
void tcp_ipv4_dump(struct tty *tty)
{
	static const char *sn[] = { "CLOSED",  "LISTEN",  "SYN_SNT", "SYN_RCV",
				    "ESTAB",   "FIN_W1",  "FIN_W2",  "CLS_WT",
				    "CLOSING", "LST_ACK", "TM_WAIT" };
	uint64_t now = timer_ticks();
	int shown = 0;

	for (int pass = 0; pass < 2; pass++) {
		int nb = pass ? TCP_EHASH_SIZE : TCP_LHASH_SIZE;

		for (int b = 0; b < nb && shown < 512; b++) {
			for (struct tcp_skc *s = pass ? g_ehash[b] : g_lhash[b];
			     s && shown < 512; s = s->skc_next) {
				uint32_t li = s->local_ip, ri = s->remote_ip;
				int st = s->state;
				const char *name = st >= 0 && st <= 10 ? sn[st] :
									 "???";

				shown++;
				if (s->skc_kind == TCP_SKC_REQ) {
					tty_printf(tty, "req %u.%u.%u.%u:%u %u.%u.%u.%u:%u\n",
						   li >> 24, (li >> 16) & 0xff,
						   (li >> 8) & 0xff, li & 0xff,
						   s->local_port, ri >> 24,
						   (ri >> 16) & 0xff,
						   (ri >> 8) & 0xff, ri & 0xff,
						   s->remote_port);
					continue;
				}
				if (s->skc_kind == TCP_SKC_TW) {
					tty_printf(tty, "tw  %u.%u.%u.%u:%u %u.%u.%u.%u:%u %s\n",
						   li >> 24, (li >> 16) & 0xff,
						   (li >> 8) & 0xff, li & 0xff,
						   s->local_port, ri >> 24,
						   (ri >> 16) & 0xff,
						   (ri >> 8) & 0xff, ri & 0xff,
						   s->remote_port, name);
					continue;
				}
				const tcp_conn_t *c = (const tcp_conn_t *)s;
				uint64_t idle_ms =
					now > c->last_rx_tick ?
						tcp_ticks_to_ms(now -
								c->last_rx_tick) :
						0;

				tty_printf(
					tty,
					"%-7s %u.%u.%u.%u:%u %u.%u.%u.%u:%u cw=%u ss=%u po=%u snd_wnd=%u rcvbuf=%d rmem=%u sndbuf=%d wmem=%u ws=%u/%u srtt=%u rto=%u bo=%u rnxt=%u snxt=%u suna=%u inq=%u ooo=%d rxr=%d txr=%d idle=%llums ackp=%u q=%u\n",
					name, li >> 24, (li >> 16) & 0xff,
					(li >> 8) & 0xff, li & 0xff,
					s->local_port, ri >> 24,
					(ri >> 16) & 0xff, (ri >> 8) & 0xff,
					ri & 0xff, s->remote_port, c->cwnd,
					c->ssthresh, c->packets_out, c->snd_wnd,
					c->rcvbuf, c->rmem_alloc, c->sndbuf,
					c->wmem_queued, c->rcv_wscale,
					c->snd_wscale, c->srtt_us, c->rto_us,
					c->rto_backoff, c->rcv_nxt, c->snd_nxt,
					c->snd_una, c->rcvq_bytes,
					c->ooo_queue.rb_node != NULL,
					c->rx_ready, c->tx_ready,
					(unsigned long long)idle_ms,
					c->ack_pending, c->ack_quick);
			}
		}
	}
}
