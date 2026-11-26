/* TCP tests of the network harness, wave 6: the SACK scoreboard, RACK,
 * TLP, PRR, F-RTO and Eifel undo, CUBIC and HyStart++, pacing, ECN,
 * path-MTU reduction mid-transfer, GRO segment counting, TCP_INFO,
 * TCP_CONGESTION, and connect() over a TIME_WAIT tuple.
 *
 * Kernel-side probes are in host/net/kt_tcp2.c (n0_kt2_*); the raw peer
 * (a made-up host whose segments are built here) as in t_tcp.c.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "tests.h"
#include "pkt.h"
#include "kt_tcp.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

extern char n0_kt_msg[256];
int n0_kt2_sacktag(void);
int n0_kt2_sacktag_split(void);
int n0_kt2_cubic_root(void);
int n0_kt2_cubic_begin(unsigned wmax, unsigned *cwnd, unsigned *ssthresh);
unsigned n0_kt2_cubic_round(void);
unsigned n0_kt2_cubic_loss(unsigned cwnd, unsigned *ssth);
void n0_kt2_cubic_state(unsigned *wmax, unsigned *K);
void n0_kt2_cubic_end(void);
int n0_kt2_prr(int nlost, int nack, int *sent, unsigned *ssthresh);
unsigned long long n0_kt_tcp_xmib(const char *name);
unsigned long long n1_kt_tcp_xmib(const char *name);
unsigned n0_kt_tcp_tw(void);
int n0_kt_tcp_sockinfo(int sock, struct kt_tcp_sockinfo *o);
int n1_kt_tcp_sockinfo(int sock, struct kt_tcp_sockinfo *o);
void n0_kt_gro_enable(int on);
void n0_kt_rx_batch(const unsigned char *buf, const unsigned *lens, int n);
int n0_kt2_rcvq_mem(int sock, unsigned *o);
unsigned long long n0_kt_gro_merged(void);

#define K_TCP_INFO 11
#define K_TCP_CONGESTION 13
#define K_SO_REUSEADDR 2
#define CA_OPEN 0
#define CA_CWR 2
#define CA_RECOVERY 3
#define CA_LOSS 4
#define F_ECE 0x40
#define F_CWR 0x80
#define INF_SSTHRESH 0x7FFFFFFFu

static unsigned long long xmib(int inst, const char *name)
{
	return inst ? n1_kt_tcp_xmib(name) : n0_kt_tcp_xmib(name);
}

/* TCP_INFO as the common ABI lays it out (independent of the kernel's
 * header: the offsets are checked against the ABI below). */
struct abi_tcp_info {
	uint8_t state, ca_state, retransmits, probes, backoff, options;
	uint8_t wscales, app_limited;
	uint32_t rto, ato, snd_mss, rcv_mss;
	uint32_t unacked, sacked, lost, retrans, fackets;
	uint32_t last_data_sent, last_ack_sent, last_data_recv, last_ack_recv;
	uint32_t pmtu, rcv_ssthresh, rtt, rttvar, snd_ssthresh, snd_cwnd;
	uint32_t advmss, reordering, rcv_rtt, rcv_space, total_retrans;
	uint64_t pacing_rate, max_pacing_rate, bytes_acked, bytes_received;
	uint32_t segs_out, segs_in, notsent_bytes, min_rtt;
	uint32_t data_segs_in, data_segs_out;
	uint64_t delivery_rate, busy_time, rwnd_limited, sndbuf_limited;
	uint32_t delivered, delivered_ce;
	uint64_t bytes_sent, bytes_retrans;
	uint32_t dsack_dups, reord_seen, rcv_ooopack, snd_wnd, rcv_wnd;
	uint32_t rehash;
	uint16_t total_rto, total_rto_recoveries;
	uint32_t total_rto_time;
};
_Static_assert(offsetof(struct abi_tcp_info, rto) == 8, "rto");
_Static_assert(offsetof(struct abi_tcp_info, total_retrans) == 100, "tr");
_Static_assert(offsetof(struct abi_tcp_info, pacing_rate) == 104, "pacing");
_Static_assert(offsetof(struct abi_tcp_info, segs_out) == 136, "segs_out");
_Static_assert(offsetof(struct abi_tcp_info, delivery_rate) == 160, "dr");
_Static_assert(offsetof(struct abi_tcp_info, delivered) == 192, "delivered");
_Static_assert(offsetof(struct abi_tcp_info, bytes_sent) == 200, "bytes_sent");
_Static_assert(offsetof(struct abi_tcp_info, snd_wnd) == 228, "snd_wnd");
_Static_assert(offsetof(struct abi_tcp_info, total_rto) == 240, "total_rto");
_Static_assert(sizeof(struct abi_tcp_info) == 248, "size");

static int tinfo(int inst, int s, struct abi_tcp_info *ti)
{
	unsigned len = sizeof(*ti);

	memset(ti, 0, sizeof(*ti));
	return NS[inst]->getsockopt(s, K_IPPROTO_TCP, K_TCP_INFO, ti, &len);
}

static int setint(int inst, int s, int level, int name, int v)
{
	return NS[inst]->setsockopt(s, level, name, &v, sizeof(v));
}

/* ======================================================================
 * The raw peer: a made-up host 10.0.0.(20+k) talking to instance 0.
 * ====================================================================== */
struct rp {
	uint32_t ip;
	uint8_t mac[6];
	uint16_t port, lport;
	uint32_t iss, risn, ts, tsecr;
	int use_ts;
	uint8_t tos; // the IP TOS of what we send (ECN field)
};

static void rp_init(struct rp *p, int k, uint16_t port, uint16_t lport)
{
	memset(p, 0, sizeof(*p));
	p->ip = (10u << 24) | (uint32_t)(20 + k);
	p->mac[0] = 0x02;
	p->mac[5] = (uint8_t)(0x20 + k);
	p->port = port;
	p->lport = lport;
	p->iss = 2000000u + (uint32_t)k * 7919u;
	p->ts = 1000;
	p->use_ts = 1;
	NS[0]->arp_add(p->ip, p->mac);
}

static void rp_send(struct rp *p, uint8_t flags, uint32_t seq, uint32_t ack,
		    unsigned len, uint16_t win, int synopts, int nsack,
		    const uint32_t *sack)
{
	uint8_t f[1700], data[1500], m0[6];
	struct tcpseg s;
	unsigned n;

	inst_mac(0, m0);
	tcpseg_init(&s);
	s.sip = p->ip;
	s.dip = inst_ip(0);
	s.sport = p->port;
	s.dport = p->lport;
	s.seq = seq;
	s.ack = ack;
	s.flags = flags;
	s.win = win;
	if (synopts) {
		s.mss = 1460;
		s.ws = 7;
		s.sackok = 1;
	}
	if (p->use_ts) {
		s.ts = 1;
		s.tsval = p->ts;
		s.tsecr = p->tsecr;
	}
	s.nsack = nsack;
	for (int i = 0; i < nsack; i++) {
		s.sack[i][0] = sack[2 * i];
		s.sack[i][1] = sack[2 * i + 1];
	}
	if (len > sizeof(data))
		len = sizeof(data);
	pat_fill(77, seq - (p->iss + 1), data, len);
	s.data = data;
	s.len = len;
	n = pkt_tcp(f, m0, p->mac, &s);
	if (p->tos) {
		f[15] = p->tos;
		frame_fix_csums(f, n);
	}
	NS[0]->rx(f, n);
}

static void rp_tx(struct rp *p, uint8_t flags, uint32_t seq, uint32_t ack,
		  unsigned len, uint16_t win, int synopts)
{
	rp_send(p, flags, seq, ack, len, win, synopts, 0, NULL);
}

/* The next segment instance 0 sends this peer; 0 when none came.  *tos:
 * its IP TOS byte. */
static int rp_rx_tos(struct rp *p, struct tcpseg *sg, simtime_t wait,
		     uint8_t *tos)
{
	uint8_t f[1700];
	simtime_t end = now_ns() + wait;

	for (;;) {
		simtime_t left = end > now_ns() ? end - now_ns() : 0;
		unsigned n = cap_next_proto(6, f, sizeof(f), left);

		if (!n)
			return 0;
		if (pkt_parse_tcp(f, n, sg) == 0 && sg->dip == p->ip &&
		    sg->dport == p->port) {
			if (sg->ts)
				p->tsecr = sg->tsval;
			if (tos)
				*tos = f[15];
			return 1;
		}
		if (!left)
			return 0;
	}
}

static int rp_rx(struct rp *p, struct tcpseg *sg, simtime_t wait)
{
	return rp_rx_tos(p, sg, wait, NULL);
}

/* Handshake toward instance 0's listener on `lport'; the ACK after
 * `rtt' (the RTT instance 0 measures).  `synflags': extra SYN flags. */
static int rp_connect_rtt(struct rp *p, simtime_t rtt, uint8_t synflags,
			  uint8_t *synack_flags)
{
	struct tcpseg sg;

	rp_tx(p, F_SYN | synflags, p->iss, 0, 0, 65535, 1);
	if (!rp_rx(p, &sg, 100 * MS) || !(sg.flags & F_SYN) ||
	    !(sg.flags & F_ACK))
		return -1;
	if (synack_flags)
		*synack_flags = sg.flags;
	p->risn = sg.seq;
	sim_run_for(rtt);
	p->ts += (uint32_t)(rtt / MS) + 1;
	rp_tx(p, F_ACK, p->iss + 1, p->risn + 1, 0, 2048, 0);
	return 0;
}

static int rp_server(struct rp *p, int k, simtime_t rtt, uint8_t synflags,
		     uint8_t *synack_flags, int *ls, int *a)
{
	*ls = NS[0]->socket(K_SOCK_STREAM);
	NS[0]->set_nonblock(*ls, 1);
	if (NS[0]->bind(*ls, 0, 8080) || NS[0]->listen(*ls, 16))
		return -1;
	cap_start(0, 1);
	rp_init(p, k, (uint16_t)(34000 + k), 8080);
	if (rp_connect_rtt(p, rtt, synflags, synack_flags) < 0)
		return -1;
	sim_run_for(2 * MS);
	*a = NS[0]->accept(*ls, NULL, NULL);
	if (*a < 0)
		return -1;
	NS[0]->set_nonblock(*a, 1);
	return 0;
}

// Collect up to `max' data segments instance 0 sends within `wait'.
static int rp_collect(struct rp *p, struct tcpseg *v, int max, simtime_t wait)
{
	int n = 0;
	simtime_t end = now_ns() + wait;

	while (n < max && now_ns() < end) {
		struct tcpseg sg;

		if (!rp_rx(p, &sg, end - now_ns()))
			break;
		if (sg.len)
			v[n++] = sg;
	}
	return n;
}

/* ======================================================================
 * Taps for interop tests
 * ====================================================================== */
#define TAPMAX 200000
static struct tseg {
	int inst;
	simtime_t t;
	uint32_t seq, ack;
	uint8_t flags, tos;
	unsigned len;
} tv[TAPMAX];
static int tn;

static int tap_fn2(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	struct tcpseg s;

	(void)ctx;
	if (pkt_parse_tcp(f, len, &s) != 0)
		return 0;
	if (tn < TAPMAX) {
		struct tseg *t = &tv[tn++];

		t->inst = inst;
		t->t = now_ns();
		t->seq = s.seq;
		t->ack = s.ack;
		t->flags = s.flags;
		t->len = s.len;
		t->tos = f[15];
	}
	return 0;
}

static void tap2_on(void)
{
	tn = 0;
	wire_set_tap(tap_fn2, NULL);
}

static void tap2_off(void)
{
	wire_set_tap(NULL, NULL);
}

static int recv_all(int inst, int s, uint8_t *buf, int want, simtime_t wait,
		    unsigned id, uint64_t off)
{
	int got = 0;
	simtime_t end = now_ns() + wait;

	while (got < want && now_ns() < end) {
		int n = NS[inst]->recv(s, buf + got, (unsigned)(want - got), 0);

		if (n > 0)
			got += n;
		else
			sim_run_for(MS);
	}
	if (got == want && pat_check(id, off, buf, (size_t)got))
		return -2;
	return got;
}

/* ======================================================================
 * The scoreboard (kernel side)
 * ====================================================================== */
static int t_sacktag(void)
{
	int r = n0_kt2_sacktag();

	CHECK(r == 0, "kt_tcp2.c:%d: %s", r, n0_kt_msg);
	r = n0_kt2_sacktag_split();
	CHECK(r == 0, "kt_tcp2.c:%d: %s", r, n0_kt_msg);
	return 0;
}

/* Reneging: the peer SACKed segments 2-4, then a cumulative ACK stops at
 * segment 2 -- it dropped what it SACKed.  The RTO comes early (half an
 * RTT, at least 10 ms), the SACK tags are cleared and segment 2 is
 * resent. */
static int t_sack_reneging(void)
{
	struct rp p;
	struct tcpseg v[16], sg;
	int ls, a, n;
	static uint8_t buf[6000];
	uint32_t base;

	CHECK(rp_server(&p, 1, 20 * MS, 0, NULL, &ls, &a) == 0, "setup");
	base = p.risn + 1;
	pat_fill(5, 0, buf, sizeof(buf));
	CHECK(NS[0]->send(a, buf, 4 * 1448, 0) == 4 * 1448, "send");
	n = rp_collect(&p, v, 16, 5 * MS);
	CHECK(n == 4, "%d segments", n);
	sim_run_for(20 * MS);
	{
		uint32_t b[2] = { base + 1448, base + 4 * 1448 };

		p.ts += 20;
		rp_send(&p, F_ACK, p.iss + 1, base, 0, 2048, 0, 1, b);
	}
	sim_run_for(MS);
	unsigned long long ren0 = xmib(0, "TCPSACKReneging");
	p.ts += 1;
	rp_tx(&p, F_ACK, p.iss + 1, base + 1448, 0, 2048, 0); // stops at seg 2
	simtime_t t0 = now_ns();
	int resent = 0;
	while (rp_rx(&p, &sg, 100 * MS)) {
		if (sg.len && sg.seq == base + 1448) {
			resent = 1;
			break;
		}
	}
	CHECK(resent, "segment 2 not resent");
	CHECK(now_ns() - t0 < 60 * MS, "resent after %llu ms (a full RTO)",
	      (now_ns() - t0) / MS);
	CHECK(xmib(0, "TCPSACKReneging") == ren0 + 1, "reneging not counted");
	return 0;
}

/* ======================================================================
 * Loss recovery (interop)
 * ====================================================================== */

/* A response whose last two segments are lost: the tail loss probe
 * (after about two RTTs) brings the SACK that repairs them, no RTO. */
static uint32_t tlp_first;
static int tlp_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	struct tcpseg s;
	static uint32_t dropped[4];
	static int nd;

	(void)ctx;
	if (!f) {
		nd = 0;
		return 0;
	}
	if (inst != 1 || pkt_parse_tcp(f, len, &s) != 0 || !s.len)
		return 0;
	if (!tlp_first)
		tlp_first = s.seq;
	uint32_t off = s.seq - tlp_first;

	if (off >= 8 * 1448 && off < 10 * 1448) {
		for (int i = 0; i < nd; i++)
			if (dropped[i] == s.seq)
				return 0;
		if (nd < 4)
			dropped[nd++] = s.seq;
		return 1;
	}
	return 0;
}

static int t_tlp_recovers_tail(void)
{
	int c, s;
	static uint8_t buf[20000], rb[20000];
	struct path_cfg p = { 0 };

	p.delay_ms = 20;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	CHECK(transfer(1, s, 0, c, 3, 100000, 5 * SEC) == 100000, "warm-up");
	sim_run_for(200 * MS);
	unsigned long long to0 = xmib(1, "TCPTimeouts");
	unsigned long long tlp0 = xmib(1, "TCPLossProbes");
	tlp_first = 0;
	tlp_tap(0, NULL, 0, NULL);
	wire_set_tap(tlp_tap, NULL);
	pat_fill(3, 0, buf, 14480);
	simtime_t t0 = now_ns();
	CHECK(NS[1]->send(s, buf, 14480, 0) == 14480, "send");
	int got = recv_all(0, c, rb, 14480, 3 * SEC, 3, 0);
	simtime_t took = now_ns() - t0;
	wire_set_tap(NULL, NULL);
	CHECK(got == 14480, "received %d (intact: %s)", got,
	      got == -2 ? "no" : "-");
	CHECK(xmib(1, "TCPLossProbes") > tlp0, "no loss probe");
	CHECK(xmib(1, "TCPTimeouts") == to0, "an RTO fired");
	CHECK(took < 250 * MS, "tail repaired after %llu ms", took / MS);
	return 0;
}

/* RACK tolerates reordering: frames held back 3 ms (well inside a
 * quarter of the 40 ms RTT) cause at most the first spurious recovery,
 * which D-SACK undoes; later ones are waited out. */
static int t_rack_reorder(void)
{
	int c, s;
	struct path_cfg p = { 0 };
	struct abi_tcp_info ti;

	p.delay_ms = 20;
	link_set_both(&p);
	// no bottleneck and no loss: reordering is all that happens
	link_cfg(0)->path[0].reorder = 0.02;
	link_cfg(0)->path[0].reorder_ms = 3;
	CHECK(tcp_pair(1, 0, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	unsigned long long r0 = NS[0]->counter(NSC_TCP_RETRANS_SEGS);
	struct flow f;

	flow_init(&f, 0, s, 1, c, 7, 20ULL * 1000 * 1000);
	CHECK(flows_run(&f, 1, 60 * SEC) == 1, "transfer incomplete");
	unsigned long long retr = NS[0]->counter(NSC_TCP_RETRANS_SEGS) - r0;
	unsigned long long reord = link_stats(0)->reordered;
	CHECK(tinfo(0, s, &ti) == 0, "TCP_INFO");
	tlog("reordered %llu, retransmitted %llu, reord_seen %u, degree %u, "
	     "dsack undo %llu",
	     reord, retr, ti.reord_seen, ti.reordering,
	     xmib(0, "TCPDSACKUndo"));
	CHECK(reord > 100, "only %llu frames reordered", reord);
	CHECK(ti.reord_seen > 0, "no reordering seen");
	CHECK(retr * 10 < reord, "%llu retransmissions for %llu reorderings",
	      retr, reord);
	return 0;
}

/* PRR (RFC 6937) send counts: one loss in a window of 20, Reno (ssthresh
 * 10): every ACK SACKs one segment; the 19 ACKs let out 10 segments, one
 * every other ACK, the first being the fast retransmit.  With many
 * losses (pipe below ssthresh) PRR-SSRB sends at most one more than was
 * delivered per ACK, and the window ends at ssthresh. */
static int t_prr_counts(void)
{
	int sent[64], total = 0, mx = 0, cw;
	unsigned ssth;

	cw = n0_kt2_prr(1, 19, sent, &ssth);
	CHECK(cw >= 0 && ssth == 10, "ssthresh %u", ssth);
	tlog("one loss: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d -> cwnd %d",
	     sent[0], sent[1], sent[2], sent[3], sent[4], sent[5], sent[6],
	     sent[7], sent[8], sent[9], sent[10], sent[11], sent[12],
	     sent[13], sent[14], sent[15], sent[16], sent[17], sent[18], cw);
	for (int i = 0; i < 19; i++) {
		total += sent[i];
		if (sent[i] > mx)
			mx = sent[i];
	}
	CHECK(sent[0] == 1, "no fast retransmit on the first ACK (%d)",
	      sent[0]);
	CHECK(total == 10 && mx == 1, "sent %d in 19 ACKs (at most %d per ACK)",
	      total, mx);
	/* proportional while the flight is above ssthresh: one segment
	 * every other ACK */
	for (int i = 0; i < 14; i++)
		CHECK(sent[i] == !(i & 1), "ACK %d let out %d", i, sent[i]);
	CHECK(cw == 10, "cwnd %d at the end of the recovery", cw);
	// heavy loss: 12 of 20 lost, pipe below ssthresh at once
	total = 0;
	cw = n0_kt2_prr(12, 8, sent, &ssth);
	for (int i = 0; i < 8; i++) {
		CHECK(sent[i] <= 2, "ACK %d let out %d", i, sent[i]);
		total += sent[i];
	}
	CHECK(cw <= (int)ssth && cw >= (int)ssth - 2,
	      "cwnd %d at the end, ssthresh %u", cw, ssth);
	return 0;
}

/* F-RTO (RFC 5682): an RTO that was spurious -- the ACKs were only
 * delayed -- is detected when the first ACK after it acknowledges data
 * that was never resent; the window is restored.  No timestamps, so
 * Eifel cannot be what undoes it. */
static int t_frto_spurious(void)
{
	struct rp p;
	struct tcpseg v[32];
	struct abi_tcp_info ti;
	int ls, a, n;
	static uint8_t buf[40000];
	uint32_t base;

	memset(&p, 0, sizeof(p));
	{
		// no timestamps on this peer
		int k = 2;

		ls = NS[0]->socket(K_SOCK_STREAM);
		NS[0]->set_nonblock(ls, 1);
		CHECK(NS[0]->bind(ls, 0, 8080) == 0 && NS[0]->listen(ls, 16) == 0,
		      "listen");
		cap_start(0, 1);
		rp_init(&p, k, 34002, 8080);
		p.use_ts = 0;
		CHECK(rp_connect_rtt(&p, 50 * MS, 0, NULL) == 0, "connect");
		sim_run_for(2 * MS);
		a = NS[0]->accept(ls, NULL, NULL);
		CHECK(a >= 0, "accept");
		NS[0]->set_nonblock(a, 1);
	}
	base = p.risn + 1;
	pat_fill(5, 0, buf, sizeof(buf));
	CHECK(NS[0]->send(a, buf, 18 * 1460, 0) == 18 * 1460, "send");
	n = rp_collect(&p, v, 32, 5 * MS);
	CHECK(n == 10, "%d segments in the initial window", n);
	unsigned long long to0 = xmib(0, "TCPTimeouts");
	unsigned long long sp0 = xmib(0, "TCPSpuriousRTOs");
	// silence: the probe, then the RTO and its resend of segment 1
	simtime_t t0 = now_ns();
	int rto = 0;
	while (now_ns() - t0 < 2 * SEC) {
		struct tcpseg sg;

		if (!rp_rx(&p, &sg, 2 * SEC - (now_ns() - t0)))
			break;
		if (sg.len && sg.seq == base && xmib(0, "TCPTimeouts") > to0) {
			rto = 1;
			break;
		}
	}
	CHECK(rto, "no RTO resend of the head");
	CHECK(tinfo(0, a, &ti) == 0 && ti.ca_state == CA_LOSS,
	      "state %u after the RTO", ti.ca_state);
	// the delayed ACKs of the originals arrive: segments 1..4
	rp_tx(&p, F_ACK, p.iss + 1, base + 4 * 1460, 0, 2048, 0);
	sim_run_for(5 * MS);
	CHECK(xmib(0, "TCPSpuriousRTOs") == sp0 + 1, "spurious RTO not detected");
	CHECK(tinfo(0, a, &ti) == 0 && ti.ca_state == CA_OPEN && ti.snd_cwnd >= 10,
	      "state %u cwnd %u after the undo", ti.ca_state, ti.snd_cwnd);
	return 0;
}

/* Eifel (RFC 3522): after an RTO, an ACK echoing the timestamp of the
 * original transmission proves the original arrived -- the timeout was
 * spurious, undone on that first ACK. */
static int t_eifel_undo(void)
{
	struct rp p;
	struct tcpseg v[32], sg;
	struct abi_tcp_info ti;
	int ls, a, n, rto = 0;
	static uint8_t buf[40000];
	uint32_t base, ts_orig;

	CHECK(rp_server(&p, 3, 50 * MS, 0, NULL, &ls, &a) == 0, "setup");
	base = p.risn + 1;
	pat_fill(5, 0, buf, sizeof(buf));
	CHECK(NS[0]->send(a, buf, 20 * 1448, 0) == 20 * 1448, "send");
	n = rp_collect(&p, v, 32, 5 * MS);
	CHECK(n == 10, "%d segments", n);
	ts_orig = v[0].tsval;
	unsigned long long to0 = xmib(0, "TCPTimeouts");
	unsigned long long lu0 = xmib(0, "TCPLossUndo");
	simtime_t t0 = now_ns();
	while (now_ns() - t0 < 2 * SEC) {
		if (!rp_rx(&p, &sg, 2 * SEC - (now_ns() - t0)))
			break;
		if (sg.len && sg.seq == base && xmib(0, "TCPTimeouts") > to0) {
			rto = 1;
			break;
		}
	}
	CHECK(rto, "no RTO");
	CHECK(tinfo(0, a, &ti) == 0 && ti.ca_state == CA_LOSS, "state %u",
	      ti.ca_state);
	// the delayed ACK of the original segment 1, echoing its TSval
	p.tsecr = ts_orig;
	p.ts += 500;
	rp_tx(&p, F_ACK, p.iss + 1, base + 1448, 0, 2048, 0);
	sim_run_for(2 * MS);
	CHECK(xmib(0, "TCPLossUndo") == lu0 + 1, "no undo");
	CHECK(tinfo(0, a, &ti) == 0 && ti.ca_state == CA_OPEN && ti.snd_cwnd >= 10,
	      "state %u cwnd %u after the undo", ti.ca_state, ti.snd_cwnd);
	return 0;
}

/* ======================================================================
 * CUBIC and HyStart++
 * ====================================================================== */
static int t_cubic_root(void)
{
	int r = n0_kt2_cubic_root();

	CHECK(r == 0, "kt_tcp2.c:%d: %s", r, n0_kt_msg);
	return 0;
}

/* The window after a loss at W_max = 100, one window of ACKs per 100 ms
 * RTT: ssthresh = 0.7 W_max, K = cbrt(W_max (1 - beta) / C) = 4.22 s;
 * concave growth up to W_max around K, convex beyond; fast convergence
 * lowers the next W_max. */
static int t_cubic_curve(void)
{
	unsigned cwnd, ssth, wmax, K, w[128];
	double tK;
	int iK, n = 0;

	CHECK(n0_kt2_cubic_begin(100, &cwnd, &ssth) == 0, "begin: %s",
	      n0_kt_msg);
	CHECK(ssth == 70 && cwnd == 70, "ssthresh %u cwnd %u", ssth, cwnd);
	for (int i = 0; i < 100; i++) {
		sim_run_for(100 * MS);
		w[n++] = n0_kt2_cubic_round();
	}
	n0_kt2_cubic_state(&wmax, &K);
	tK = K / 1024.0;
	tlog("K %.3f s, W_max %u; cwnd at 1 s %u, 2 s %u, 4 s %u, 6 s %u, 8 s %u",
	     tK, wmax, w[9], w[19], w[39], w[59], w[79]);
	CHECK(wmax == 100, "W_max %u", wmax);
	CHECK(K >= 4300 && K <= 4340, "K %u/1024 s", K);
	iK = (int)(tK * 10 + 0.5) - 1;
	CHECK(w[iK] >= 95 && w[iK] <= 105, "cwnd %u at K", w[iK]);
	CHECK(w[iK / 2] >= 85 && w[iK / 2] < 100, "cwnd %u at K/2", w[iK / 2]);
	// concave before K: the first half grows faster than the second
	CHECK(w[iK / 2] - w[0] > w[iK] - w[iK / 2], "not concave: %u %u %u",
	      w[0], w[iK / 2], w[iK]);
	// convex after K: growth speeds up again
	CHECK(w[2 * iK] - w[(3 * iK) / 2] > w[(3 * iK) / 2] - w[iK],
	      "not convex: %u %u %u", w[iK], w[(3 * iK) / 2], w[2 * iK]);
	CHECK(w[2 * iK] >= 115 && w[2 * iK] <= 145, "cwnd %u at 2K", w[2 * iK]);
	// fast convergence: a loss below the previous W_max
	wmax = n0_kt2_cubic_loss(90, &ssth);
	CHECK(wmax == 76 && ssth == 63, "fast convergence: W_max %u ssthresh %u",
	      wmax, ssth);
	n0_kt2_cubic_end();
	return 0;
}

/* HyStart++: a bottleneck with a deep buffer -- the RTT rises as the
 * queue builds in slow start, and slow start ends (CSS, then congestion
 * avoidance) before the first loss. */
static int t_hystart_exit(void)
{
	int c, s;
	struct path_cfg p = { 0 };
	struct abi_tcp_info ti;
	unsigned ss_cwnd = 0;
	unsigned long long ss_retr = ~0ULL;

	p.delay_ms = 20;
	link_set_both(&p);
	link_cfg(0)->rate_mbps = link_cfg(1)->rate_mbps = 50;
	link_cfg(0)->queue_bytes = link_cfg(1)->queue_bytes = 3 << 20;
	CHECK(tcp_pair(1, 0, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	struct flow f;

	flow_init(&f, 0, s, 1, c, 9, 30ULL * 1000 * 1000);
	unsigned long long d0 = xmib(0, "TCPHystartDelayDetect");
	// run in slices, watching for the end of slow start
	for (int i = 0; i < 400 && !f.done; i++) {
		flows_run(&f, 1, 10 * MS);
		if (tinfo(0, s, &ti) == 0 && ti.snd_ssthresh != INF_SSTHRESH &&
		    ss_retr == ~0ULL) {
			ss_retr = ti.total_retrans;
			ss_cwnd = ti.snd_cwnd;
		}
	}
	CHECK(xmib(0, "TCPHystartDelayDetect") > d0, "no delay increase seen");
	CHECK(ss_retr == 0, "slow start ended %s",
	      ss_retr == ~0ULL ? "never" : "only with a loss");
	tlog("slow start ended at cwnd %u, HyStart cwnd sum %llu", ss_cwnd,
	     xmib(0, "TCPHystartDelayCwnd"));
	return 0;
}

/* ======================================================================
 * Pacing
 * ====================================================================== */
static int t_pacing(void)
{
	int c, s;
	struct path_cfg p = { 0 };
	struct abi_tcp_info ti;

	p.delay_ms = 50;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	struct flow f;
	/* TCP paces only where asked to: a cap far above the path's rate
	 * turns it on without limiting anything. */
	unsigned long long nocap = 1ULL << 40;

	CHECK(NS[1]->setsockopt(s, K_SOL_SOCKET, 47 /* SO_MAX_PACING_RATE */,
				&nocap, sizeof(nocap)) == 0,
	      "SO_MAX_PACING_RATE");
	flow_init(&f, 1, s, 0, c, 11, 200ULL * 1000 * 1000);
	unsigned long long d0 = xmib(1, "TCPPacingDefer");
	flows_run(&f, 1, 600 * MS);
	CHECK(tinfo(1, s, &ti) == 0, "TCP_INFO");
	{
		double ratio = ti.snd_cwnd < ti.snd_ssthresh / 2 ? 2.0 : 1.2;
		unsigned out = ti.snd_cwnd > ti.unacked ? ti.snd_cwnd : ti.unacked;
		double want = ratio * out * ti.snd_mss * 1e6 / ti.rtt;

		tlog("cwnd %u ssthresh %u srtt %u us pacing %llu B/s (expected "
		     "%.0f)",
		     ti.snd_cwnd, ti.snd_ssthresh, ti.rtt,
		     (unsigned long long)ti.pacing_rate, want);
		CHECK(ti.pacing_rate > want * 0.98 && ti.pacing_rate < want * 1.02,
		      "pacing rate %llu, expected %.0f",
		      (unsigned long long)ti.pacing_rate, want);
	}
	// departures over the next 300 ms: no burst beyond two ticks' worth
	tap2_on();
	flows_run(&f, 1, 300 * MS);
	tap2_off();
	CHECK(tinfo(1, s, &ti) == 0, "TCP_INFO");
	{
		double per_tick = (double)ti.pacing_rate * 0.005 / ti.snd_mss;
		int worst = 0;

		for (int i = 0; i < tn; i++) {
			int k = 0;

			if (tv[i].inst != 1 || !tv[i].len)
				continue;
			for (int j = i; j < tn && tv[j].t < tv[i].t + 5 * MS; j++)
				if (tv[j].inst == 1 && tv[j].len)
					k++;
			if (k > worst)
				worst = k;
		}
		tlog("at most %d segments in 5 ms; %.1f per tick at the rate",
		     worst, per_tick);
		CHECK(worst <= (int)(2 * per_tick) + 3,
		      "a burst of %d segments in 5 ms (%.1f per tick)", worst,
		      per_tick);
	}
	CHECK(xmib(1, "TCPPacingDefer") > d0, "nothing was paced");
	return 0;
}

/* ======================================================================
 * ECN
 * ====================================================================== */
static int t_ecn(void)
{
	struct rp p;
	struct tcpseg sg, v[16];
	struct abi_tcp_info ti;
	int ls, a, n;
	uint8_t synack = 0, tos = 0;
	static uint8_t buf[30000];
	uint32_t base, rbase;

	// a SYN asking for ECN: the SYN-ACK says ECE, not CWR
	CHECK(rp_server(&p, 4, 20 * MS, F_ECE | F_CWR, &synack, &ls, &a) == 0,
	      "setup");
	CHECK((synack & (F_ECE | F_CWR)) == F_ECE, "SYN-ACK flags %02x", synack);
	CHECK(tinfo(0, a, &ti) == 0 && (ti.options & 8), "not negotiated");
	base = p.risn + 1;
	rbase = p.iss + 1;
	// receiver: a CE mark is echoed until the sender's CWR
	p.tos = 3; // CE
	p.ts++;
	rp_tx(&p, F_ACK | F_PSH, rbase, base, 1000, 2048, 0);
	CHECK(rp_rx(&p, &sg, 10 * MS) && (sg.flags & F_ECE),
	      "no ECE on the ACK of a CE segment (flags %02x)", sg.flags);
	p.tos = 2; // ECT(0)
	p.ts++;
	rp_tx(&p, F_ACK | F_PSH, rbase + 1000, base, 1000, 2048, 0);
	CHECK(rp_rx(&p, &sg, 300 * MS) && (sg.flags & F_ECE),
	      "ECE not repeated (flags %02x)", sg.flags);
	p.ts++;
	rp_tx(&p, F_ACK | F_PSH | F_CWR, rbase + 2000, base, 1000, 2048, 0);
	CHECK(rp_rx(&p, &sg, 300 * MS) && !(sg.flags & F_ECE),
	      "ECE after the CWR (flags %02x)", sg.flags);
	// sender: new data ECT(0), the reduction once per window, CWR once
	pat_fill(5, 0, buf, sizeof(buf));
	CHECK(NS[0]->send(a, buf, 10 * 1448, 0) == 10 * 1448, "send");
	n = 0;
	for (;;) {
		if (!rp_rx_tos(&p, &v[n], 5 * MS, &tos))
			break;
		if (!v[n].len)
			continue;
		CHECK((tos & 3) == 2, "data with ECN field %u", tos & 3);
		if (++n == 10)
			break;
	}
	CHECK(n == 10, "%d segments", n);
	p.ts++;
	rp_tx(&p, F_ACK | F_ECE, rbase + 3000, base + 2 * 1448, 0, 2048, 0);
	sim_run_for(MS);
	CHECK(tinfo(0, a, &ti) == 0 && ti.ca_state == CA_CWR, "state %u",
	      ti.ca_state);
	unsigned ssth = ti.snd_ssthresh, cw = ti.snd_cwnd;
	CHECK(ssth == 7, "ssthresh %u after the echo", ssth);
	p.ts++;
	rp_tx(&p, F_ACK | F_ECE, rbase + 3000, base + 4 * 1448, 0, 2048, 0);
	sim_run_for(MS);
	CHECK(tinfo(0, a, &ti) == 0 && ti.snd_ssthresh == ssth,
	      "reduced twice in a window: ssthresh %u", ti.snd_ssthresh);
	(void)cw;
	CHECK(NS[0]->send(a, buf, 4 * 1448, 0) == 4 * 1448, "send more");
	{
		int cwr = 0, data = 0;

		while (rp_rx_tos(&p, &sg, 20 * MS, &tos)) {
			if (!sg.len) {
				CHECK((tos & 3) == 0, "a pure ACK with ECT");
				continue;
			}
			data++;
			if (sg.flags & F_CWR)
				cwr++;
		}
		CHECK(data > 0 && cwr == 1, "%d CWR in %d new segments", cwr, data);
	}
	return 0;
}

// Without an ECN request (or with ECE alone) nothing is negotiated.
static int t_ecn_not_requested(void)
{
	struct rp p;
	int ls, a;
	uint8_t synack = 0;
	struct abi_tcp_info ti;

	CHECK(rp_server(&p, 5, 10 * MS, F_ECE, &synack, &ls, &a) == 0, "setup");
	CHECK(!(synack & (F_ECE | F_CWR)), "SYN-ACK flags %02x", synack);
	CHECK(tinfo(0, a, &ti) == 0 && !(ti.options & 8), "negotiated");
	return 0;
}

/* ======================================================================
 * Path MTU
 * ====================================================================== */
static int pm_drop_seen, pm_icmps;
static uint8_t pm_quote[64];
static unsigned pm_qlen;

static void pm_inject(void *arg, long mtu)
{
	uint8_t f[200], hymac[6] = { 2, 0, 0, 0, 0, 0x09 }, m0[6];
	unsigned n;

	(void)arg;
	inst_mac(0, m0);
	n = pkt_icmp_err(f, m0, hymac, (10u << 24) | 9, inst_ip(0), 3, 4,
			 (uint32_t)mtu, pm_quote, pm_qlen);
	NS[0]->rx(f, n);
	pm_icmps++;
}

static int pm_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	(void)ctx;
	/* The narrowed link drops what is too large; the router before it
	 * reports the first few. */
	if (inst == 0 && link_cfg(0)->mtu && len > 14 + link_cfg(0)->mtu &&
	    f[23] == 6) {
		pm_drop_seen++;
		if (pm_drop_seen <= 2) {
			pm_qlen = 28;
			memcpy(pm_quote, f + 14, pm_qlen);
			sim_after(MS, pm_inject, NULL, link_cfg(0)->mtu);
		}
	}
	return 0;
}

static int t_pmtu_midstream(void)
{
	int c, s;
	struct path_cfg p = { 0 };
	struct abi_tcp_info ti;
	uint8_t hymac[6] = { 2, 0, 0, 0, 0, 0x09 };

	p.delay_ms = 10;
	link_set_both(&p);
	NS[0]->arp_add((10u << 24) | 9, hymac);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	struct flow f;

	flow_init(&f, 0, c, 1, s, 13, 4ULL * 1000 * 1000);
	flows_run(&f, 1, 100 * MS);
	CHECK(tinfo(0, c, &ti) == 0 && ti.snd_mss == 1448, "mss %u", ti.snd_mss);
	// a report quoting a sequence number we never sent is ignored
	{
		uint8_t q[28];

		memset(q, 0, sizeof(q));
		q[0] = 0x45;
		q[2] = 0x05;
		q[3] = 0xdc;
		q[6] = 0x40;
		q[8] = 64;
		q[9] = 6;
		uint32_t sip = inst_ip(0), dip = inst_ip(1);
		for (int i = 0; i < 4; i++) {
			q[12 + i] = (uint8_t)(sip >> (24 - 8 * i));
			q[16 + i] = (uint8_t)(dip >> (24 - 8 * i));
		}
		uint16_t ck = inet_csum(q, 20, 0);
		q[10] = (uint8_t)(ck >> 8);
		q[11] = (uint8_t)ck;
		unsigned sport, dport, ip;
		NS[0]->sockname(c, 0, &ip, &sport);
		dport = 8080;
		q[20] = (uint8_t)(sport >> 8);
		q[21] = (uint8_t)sport;
		q[22] = (uint8_t)(dport >> 8);
		q[23] = (uint8_t)dport;
		q[24] = q[25] = q[26] = q[27] = 0x11; // far outside the window
		memcpy(pm_quote, q, 28);
		pm_qlen = 28;
		pm_inject(NULL, 1000);
		sim_run_for(MS);
		CHECK(tinfo(0, c, &ti) == 0 && ti.snd_mss == 1448,
		      "an out-of-window report changed the MSS to %u", ti.snd_mss);
	}
	unsigned long long to0 = xmib(0, "TCPTimeouts");
	unsigned long long mr0 = xmib(0, "TCPMTUReduced");
	pm_drop_seen = pm_icmps = 0;
	wire_set_tap(pm_tap, NULL);
	link_cfg(0)->mtu = 1200;
	int ok = flows_run(&f, 1, 20 * SEC);
	wire_set_tap(NULL, NULL);
	CHECK(ok == 1 && !f.bad, "transfer incomplete (%llu of %llu)", f.got,
	      f.n);
	CHECK(tinfo(0, c, &ti) == 0 && ti.snd_mss <= 1200 - 52 && ti.pmtu == 1200,
	      "mss %u pmtu %u", ti.snd_mss, ti.pmtu);
	CHECK(xmib(0, "TCPMTUReduced") > mr0, "not reduced");
	CHECK(xmib(0, "TCPTimeouts") == to0, "%llu RTOs",
	      xmib(0, "TCPTimeouts") - to0);
	tlog("%d oversized dropped, %d reports", pm_drop_seen, pm_icmps);
	return 0;
}

/* ======================================================================
 * GRO: a merged segment counts as the segments it holds
 * ====================================================================== */
static int t_gro_segments(void)
{
	struct rp p;
	struct tcpseg sg;
	struct kt_tcp_sockinfo si;
	struct abi_tcp_info ti;
	int ls, a, acks = 0;
	static uint8_t frames[16][1600], batch[16 * 1600], rb[20000];
	unsigned lens[16], off = 0;
	uint32_t rbase, base;
	uint8_t m0[6];

	CHECK(rp_server(&p, 6, 10 * MS, 0, NULL, &ls, &a) == 0, "setup");
	rbase = p.iss + 1;
	base = p.risn + 1;
	n0_kt_gro_enable(1);
	inst_mac(0, m0);
	CHECK(tinfo(0, a, &ti) == 0, "TCP_INFO");
	unsigned segs0 = ti.segs_in, dsegs0 = ti.data_segs_in;
	p.ts += 5;
	for (int i = 0; i < 10; i++) {
		struct tcpseg s;
		uint8_t data[1448];

		tcpseg_init(&s);
		s.sip = p.ip;
		s.dip = inst_ip(0);
		s.sport = p.port;
		s.dport = p.lport;
		s.seq = rbase + (uint32_t)i * 1448;
		s.ack = base;
		s.flags = F_ACK;
		s.win = 2048;
		s.ts = 1;
		s.tsval = p.ts;
		s.tsecr = p.tsecr;
		pat_fill(21, (uint64_t)i * 1448, data, sizeof(data));
		s.data = data;
		s.len = sizeof(data);
		lens[i] = pkt_tcp(frames[i], m0, p.mac, &s);
		memcpy(batch + off, frames[i], lens[i]);
		off += lens[i];
	}
	n0_kt_rx_batch(batch, lens, 10);
	while (rp_rx(&p, &sg, 50 * MS))
		if (!sg.len && sg.ack == rbase + 10 * 1448)
			acks++;
	n0_kt_gro_enable(0);
	CHECK(acks >= 1 && acks <= 2, "%d ACKs for one merged segment", acks);
	CHECK(n0_kt_tcp_sockinfo(a, &si) == 0 && si.rcv_mss == 1448,
	      "rcv_mss %u", si.rcv_mss);
	CHECK(tinfo(0, a, &ti) == 0 && ti.segs_in - segs0 == 10 &&
		      ti.data_segs_in - dsegs0 == 10,
	      "segments in %u, data %u", ti.segs_in - segs0,
	      ti.data_segs_in - dsegs0);
	CHECK(recv_all(0, a, rb, 14480, SEC, 21, 0) == 14480, "data");
	return 0;
}

/* ======================================================================
 * Socket options
 * ====================================================================== */
static int t_tcp_info_fields(void)
{
	int c, s;
	struct path_cfg p = { 0 };
	struct abi_tcp_info ti;
	unsigned len;

	p.delay_ms = 15;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	CHECK(transfer(1, s, 0, c, 4, 200000, 5 * SEC) == 200000, "transfer");
	sim_run_for(100 * MS);
	CHECK(tinfo(1, s, &ti) == 0, "TCP_INFO");
	tlog("rtt %u min_rtt %u cwnd %u pacing %llu delivery %llu segs_out %u "
	     "acked %llu sent %llu",
	     ti.rtt, ti.min_rtt, ti.snd_cwnd, (unsigned long long)ti.pacing_rate,
	     (unsigned long long)ti.delivery_rate, ti.segs_out,
	     (unsigned long long)ti.bytes_acked,
	     (unsigned long long)ti.bytes_sent);
	CHECK(ti.state == 1 /* ABI: ESTABLISHED */ && ti.ca_state == CA_OPEN,
	      "state %u/%u",
	      ti.state, ti.ca_state);
	CHECK((ti.options & 7) == 7, "options %x", ti.options);
	CHECK(ti.snd_mss == 1448 && ti.advmss == 1460 && ti.pmtu == 1500,
	      "mss %u advmss %u pmtu %u", ti.snd_mss, ti.advmss, ti.pmtu);
	CHECK(ti.min_rtt >= 29000 && ti.min_rtt <= 31000, "min_rtt %u", ti.min_rtt);
	CHECK(ti.rtt >= ti.min_rtt && ti.rtt < 60000, "rtt %u", ti.rtt);
	CHECK(ti.bytes_acked == 200000 && ti.bytes_sent == 200000,
	      "bytes acked %llu sent %llu", (unsigned long long)ti.bytes_acked,
	      (unsigned long long)ti.bytes_sent);
	CHECK(ti.data_segs_out >= 139 && ti.segs_out >= ti.data_segs_out,
	      "segments out %u/%u", ti.data_segs_out, ti.segs_out);
	CHECK(ti.delivered >= 139, "delivered %u", ti.delivered);
	CHECK(ti.delivery_rate > 0 && ti.pacing_rate > 0 &&
		      ti.pacing_rate != ~0ULL,
	      "rates %llu %llu", (unsigned long long)ti.delivery_rate,
	      (unsigned long long)ti.pacing_rate);
	CHECK(ti.reordering == 3 && ti.total_retrans == 0 && ti.unacked == 0,
	      "reordering %u retrans %u unacked %u", ti.reordering,
	      ti.total_retrans, ti.unacked);
	CHECK(ti.snd_wnd > 0 && ti.notsent_bytes == 0, "snd_wnd %u notsent %u",
	      ti.snd_wnd, ti.notsent_bytes);
	CHECK(tinfo(0, c, &ti) == 0 && ti.bytes_received == 200000 &&
		      ti.data_segs_in >= 139 && ti.rcv_wnd > 0,
	      "receiver: bytes %llu segs %u rcv_wnd %u",
	      (unsigned long long)ti.bytes_received, ti.data_segs_in, ti.rcv_wnd);
	// a shorter buffer gets the prefix of the struct
	{
		uint8_t small[104];

		len = sizeof(small);
		CHECK(NS[1]->getsockopt(s, K_IPPROTO_TCP, K_TCP_INFO, small, &len) ==
				      0 &&
			      len == 104,
		      "short TCP_INFO: len %u", len);
	}
	return 0;
}

static int getca(int inst, int s, char *name)
{
	unsigned len = 16;

	memset(name, 0, 17);
	return NS[inst]->getsockopt(s, K_IPPROTO_TCP, K_TCP_CONGESTION, name, &len);
}

static int t_tcp_congestion(void)
{
	char name[17];
	int c, s, ls = NS[1]->socket(K_SOCK_STREAM);

	NS[1]->set_nonblock(ls, 1);
	CHECK(getca(1, ls, name) == 0 && !strcmp(name, "cubic"),
	      "default '%s'", name);
	CHECK(NS[1]->setsockopt(ls, K_IPPROTO_TCP, K_TCP_CONGESTION, "bogus", 5) ==
		      -ENOENT,
	      "unknown name accepted");
	CHECK(NS[1]->setsockopt(ls, K_IPPROTO_TCP, K_TCP_CONGESTION, "reno", 4) == 0,
	      "set reno");
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 8) == 0,
	      "listen");
	c = NS[0]->socket(K_SOCK_STREAM);
	NS[0]->set_nonblock(c, 1);
	// before connect(), with a terminating NUL in the length
	CHECK(NS[0]->setsockopt(c, K_IPPROTO_TCP, K_TCP_CONGESTION, "reno\0", 5) ==
		      0,
	      "set reno before connect");
	NS[0]->connect(c, inst_ip(1), 8080);
	CHECK(wait_established(0, c, 2 * SEC) == 0, "connect");
	sim_run_for(10 * MS);
	s = NS[1]->accept(ls, NULL, NULL);
	CHECK(s >= 0, "accept");
	NS[1]->set_nonblock(s, 1);
	CHECK(getca(0, c, name) == 0 && !strcmp(name, "reno"), "client '%s'",
	      name);
	CHECK(getca(1, s, name) == 0 && !strcmp(name, "reno"),
	      "child of a reno listener: '%s'", name);
	// switch on an established connection, and back
	CHECK(NS[0]->setsockopt(c, K_IPPROTO_TCP, K_TCP_CONGESTION, "cubic", 5) ==
		      0 &&
		      getca(0, c, name) == 0 && !strcmp(name, "cubic"),
	      "switch to cubic: '%s'", name);
	CHECK(transfer(0, c, 1, s, 8, 100000, 5 * SEC) == 100000,
	      "data after the switch");
	CHECK(NS[0]->setsockopt(c, K_IPPROTO_TCP, K_TCP_CONGESTION, "reno", 4) ==
			      0 &&
		      getca(0, c, name) == 0 && !strcmp(name, "reno"),
	      "back to reno: '%s'", name);
	{
		unsigned len = 3;

		CHECK(NS[0]->getsockopt(c, K_IPPROTO_TCP, K_TCP_CONGESTION, name,
					&len) == 0 &&
			      len == 3 && !memcmp(name, "ren", 3),
		      "short buffer: %u", len);
	}
	return 0;
}

/* connect() to a 4-tuple a TIME_WAIT sock holds that cannot be reused
 * (the old connection had no timestamps): EADDRNOTAVAIL. */
static int t_tw_eaddrnotavail(void)
{
	struct rp p;
	struct tcpseg sg;
	int c, r;

	rp_init(&p, 7, 9090, 46000);
	p.use_ts = 0;
	cap_start(0, 1);
	c = NS[0]->socket(K_SOCK_STREAM);
	NS[0]->set_nonblock(c, 1);
	setint(0, c, K_SOL_SOCKET, K_SO_REUSEADDR, 1);
	CHECK(NS[0]->bind(c, 0, 46000) == 0, "bind");
	r = NS[0]->connect(c, p.ip, 9090);
	CHECK(r == -EINPROGRESS || r == 0, "connect %d", r);
	CHECK(rp_rx(&p, &sg, 10 * MS) && sg.flags == F_SYN, "no SYN");
	p.risn = sg.seq;
	rp_tx(&p, F_SYN | F_ACK, p.iss, p.risn + 1, 0, 65535, 1);
	CHECK(rp_rx(&p, &sg, 10 * MS) && sg.flags == F_ACK, "no ACK");
	CHECK(NS[0]->tcp_state(c) == TS_ESTABLISHED, "not established");
	NS[0]->close(c);
	CHECK(rp_rx(&p, &sg, 10 * MS) && (sg.flags & F_FIN), "no FIN");
	rp_tx(&p, F_FIN | F_ACK, p.iss + 1, p.risn + 2, 0, 65535, 0);
	CHECK(rp_rx(&p, &sg, 10 * MS) && sg.flags == F_ACK, "no last ACK");
	sim_run_for(10 * MS);
	CHECK(n0_kt_tcp_tw() == 1, "%u TIME_WAIT", n0_kt_tcp_tw());
	c = NS[0]->socket(K_SOCK_STREAM);
	NS[0]->set_nonblock(c, 1);
	setint(0, c, K_SOL_SOCKET, K_SO_REUSEADDR, 1);
	CHECK(NS[0]->bind(c, 0, 46000) == 0, "bind again");
	r = NS[0]->connect(c, p.ip, 9090);
	CHECK(r == -EADDRNOTAVAIL, "connect over TIME_WAIT: %d", r);
	return 0;
}

/* ======================================================================
 * Loss recovery under random loss; GRO memory
 * ====================================================================== */
// the neighbours' ARP entries, so a lost ARP frame cannot delay the start
static void arp_warm(void)
{
	unsigned char m[6];

	for (int i = 0; i < 2; i++) {
		inst_mac(i ^ 1, m);
		NS[i]->arp_add(inst_ip(i ^ 1), m);
	}
}

/* 1 % random loss both ways at 10 ms RTT: every loss is repaired by fast
 * recovery or the loss probe, never by a timeout, and nothing is resent
 * that was not lost (no spurious retransmission, no lost-retransmission
 * false alarm). */
static int t_loss1_no_rto(void)
{
	struct path_cfg p = { 0 };
	struct flow f;
	int c, s;

	arp_warm();
	p.delay_ms = 5;
	p.loss = 0.01;
	link_set_both(&p);
	link_cfg(0)->rate_mbps = link_cfg(1)->rate_mbps = 1000;
	link_cfg(0)->queue_bytes = link_cfg(1)->queue_bytes = 4 << 20;
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 30 * SEC) == 0, "pair");
	unsigned long long r0 = NS[1]->counter(NSC_TCP_RETRANS_SEGS);
	unsigned long long to0 = xmib(1, "TCPTimeouts");
	unsigned long long rec0 = xmib(1, "TCPSackRecovery");
	unsigned long long tlp0 = xmib(1, "TCPLossProbes");
	unsigned long long ds0 = xmib(1, "TCPDSACKRecv");
	unsigned long long lost0 = link_stats(1)->drop_loss;

	flow_init(&f, 1, s, 0, c, 9, 10ULL * 1000 * 1000);
	CHECK(flows_run(&f, 1, 120 * SEC) == 1, "transfer incomplete");
	unsigned long long retr = NS[1]->counter(NSC_TCP_RETRANS_SEGS) - r0;
	unsigned long long lost = link_stats(1)->drop_loss - lost0;
	unsigned long long tlp = xmib(1, "TCPLossProbes") - tlp0;
	unsigned long long rec = xmib(1, "TCPSackRecovery") - rec0;

	tlog("lost %llu, retransmitted %llu, recoveries %llu, probes %llu, "
	     "dsacks %llu",
	     lost, retr, rec, tlp, xmib(1, "TCPDSACKRecv") - ds0);
	CHECK(lost > 30, "only %llu segments lost", lost);
	CHECK(xmib(1, "TCPTimeouts") == to0, "%llu timeouts",
	      xmib(1, "TCPTimeouts") - to0);
	CHECK(retr <= lost + tlp, "%llu retransmissions for %llu losses",
	      retr, lost);
	CHECK(xmib(1, "TCPDSACKRecv") - ds0 <= tlp, "%llu spurious resends",
	      xmib(1, "TCPDSACKRecv") - ds0);
	CHECK(rec >= 1 && rec <= lost, "%llu recoveries for %llu losses", rec,
	      lost);
	return 0;
}

/* Four segments of one window lost: one recovery repairs them all -- four
 * retransmissions, one window reduction, no timeout -- within a few
 * round trips. */
static uint32_t ml_first;
static uint32_t ml_dropped[4];
static int ml_nd;

static int ml_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	static const unsigned which[4] = { 10, 13, 17, 20 };
	struct tcpseg sg;

	(void)ctx;
	if (inst != 1 || pkt_parse_tcp(f, len, &sg) != 0 || !sg.len)
		return 0;
	if (!ml_first)
		ml_first = sg.seq;
	for (int i = 0; i < 4; i++) {
		if (sg.seq != ml_first + which[i] * 1448)
			continue;
		for (int k = 0; k < ml_nd; k++)
			if (ml_dropped[k] == sg.seq)
				return 0; // the retransmission goes through
		ml_dropped[ml_nd++] = sg.seq;
		return 1;
	}
	return 0;
}

static int t_multi_loss_one_recovery(void)
{
	struct path_cfg p = { 0 };
	struct kt_tcp_sockinfo si;
	static uint8_t buf[60 * 1448], rb[60 * 1448];
	int c, s;

	p.delay_ms = 20;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	CHECK(transfer(1, s, 0, c, 3, 400000, 10 * SEC) == 400000, "warm-up");
	sim_run_for(100 * MS);
	unsigned long long r0 = NS[1]->counter(NSC_TCP_RETRANS_SEGS);
	unsigned long long to0 = xmib(1, "TCPTimeouts");
	unsigned long long rec0 = xmib(1, "TCPSackRecovery");
	CHECK(n1_kt_tcp_sockinfo(s, &si) == 0, "sockinfo");
	unsigned cwnd0 = si.cwnd;

	CHECK(cwnd0 >= 40, "cwnd %u after the warm-up", cwnd0);
	ml_first = 0;
	ml_nd = 0;
	wire_set_tap(ml_tap, NULL);
	pat_fill(4, 0, buf, sizeof(buf));
	simtime_t t0 = now_ns();
	CHECK(NS[1]->send(s, buf, sizeof(buf), 0) == (int)sizeof(buf), "send");
	int got = recv_all(0, c, rb, (int)sizeof(rb), 3 * SEC, 4, 0);
	simtime_t took = now_ns() - t0;
	wire_set_tap(NULL, NULL);
	CHECK(got == (int)sizeof(rb), "received %d", got);
	CHECK(ml_nd == 4, "%d segments dropped", ml_nd);
	CHECK(n1_kt_tcp_sockinfo(s, &si) == 0, "sockinfo");
	tlog("cwnd %u -> %u, ssthresh %u, %llu ms", cwnd0, si.cwnd,
	     si.ssthresh, took / MS);
	CHECK(NS[1]->counter(NSC_TCP_RETRANS_SEGS) - r0 == 4,
	      "%llu retransmissions",
	      NS[1]->counter(NSC_TCP_RETRANS_SEGS) - r0);
	CHECK(xmib(1, "TCPSackRecovery") - rec0 == 1, "%llu recoveries",
	      xmib(1, "TCPSackRecovery") - rec0);
	CHECK(xmib(1, "TCPTimeouts") == to0, "a timeout");
	// one reduction (beta 0.7 of the window at the loss), not two
	CHECK(si.ssthresh >= cwnd0 / 2 && si.ssthresh < 0x7FFFFFFF,
	      "ssthresh %u from cwnd %u", si.ssthresh, cwnd0);
	CHECK(took < 4 * 40 * MS, "repaired after %llu ms", took / MS);
	return 0;
}

/* GRO with a NIC that hands over frames in batches: merged segments sit
 * in the receive queue in buffers sized for them -- the queue's memory
 * stays within twice its payload (a merge in a 64 KB buffer would cost
 * several times that), and the data arrives intact. */
static int t_gro_truesize(void)
{
	struct path_cfg p = { 0 };
	static uint8_t buf[65536], rb[65536];
	unsigned o[6];
	unsigned long long sent = 0, got = 0, m0;
	int c, s;

	p.delay_ms = 5;
	link_set_both(&p);
	link_cfg(0)->rate_mbps = link_cfg(1)->rate_mbps = 1000;
	link_cfg(1)->rx_batch_us = 200; // instance 0's NIC
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	m0 = n0_kt_gro_merged();
	// the reader sleeps: the receive queue fills up to the window
	for (int i = 0; i < 400; i++) {
		pat_fill(5, sent, buf, sizeof(buf));
		int n = NS[1]->send(s, buf, sizeof(buf), 0);

		if (n > 0)
			sent += (unsigned)n;
		sim_run_for(5 * MS);
	}
	CHECK(n0_kt2_rcvq_mem(c, o) == 0, "rcvq");
	tlog("rcvq: %u skbs, %u merged, %u bytes, %u memory, largest merge "
	     "%u for %u bytes; %llu segments merged",
	     o[0], o[1], o[2], o[3], o[4], o[5], n0_kt_gro_merged() - m0);
	CHECK(n0_kt_gro_merged() > m0 + 20, "only %llu segments merged",
	      n0_kt_gro_merged() - m0);
	CHECK(o[1] > 0 && o[2] > 100000, "queue: %u merged, %u bytes", o[1],
	      o[2]);
	CHECK(o[3] <= 2 * o[2], "queue memory %u for %u bytes", o[3], o[2]);
	CHECK(o[4] <= 2 * o[5] + 4096, "a merge of %u bytes costs %u", o[5],
	      o[4]);
	// everything arrives, in order and intact
	while (got < sent) {
		int n = NS[0]->recv(c, rb, sizeof(rb), 0);

		if (n > 0) {
			CHECK(!pat_check(5, got, rb, (size_t)n), "corrupt at %llu",
			      got);
			got += (unsigned)n;
			continue;
		}
		sim_run_for(MS);
		if (now_ns() > 60 * SEC)
			break;
	}
	CHECK(got == sent, "received %llu of %llu", got, sent);
	return 0;
}

/* ======================================================================
 * The review fixes (tmp/net-review-b.md): give-up accounting of window
 * probes, the per-socket memory floor and the global out-of-order
 * reclaim, published-object invariants, the TIME_WAIT reuse policy, the
 * flight list and sequence index, single challenge ACKs, socket options
 * of a detached connection, pacing off by default and SO_MAX_PACING_RATE,
 * bounded receive-queue collapsing.
 * ====================================================================== */
#define K_TCP_USER_TIMEOUT 18
#define K_SO_MAX_PACING_RATE 47
int n0_kt3_tw_published(unsigned lip, unsigned lport, unsigned rip,
			unsigned rport);
int n0_kt3_full_published(unsigned lip, unsigned lport, unsigned rip,
			  unsigned rport);
int n1_kt3_flight_check(int sock, unsigned *n_list, unsigned *n_idx);
int n0_kt3_flight_check(int sock, unsigned *n_list, unsigned *n_idx);
int n0_kt3_twsk_unique_policy(void);
void n1_kt3_internal_pacing(int on);
int n0_kt3_rmem(int sock, unsigned *rmem, unsigned *ooo);
unsigned n0_kt_tcp_orphans(void);
unsigned n0_kt_tcp_full_socks(void);
unsigned long long n0_kt_tcp_mem(void);
extern char n1_kt_msg[256];

/* The peer of rp_server shuts its window with a pure window update; the
 * application queues 100 bytes.  Returns the accepted socket. */
static int zw_setup(struct rp *p, int k, int *ls)
{
	static uint8_t buf[100];
	int a;

	if (rp_server(p, k, 100 * MS, 0, NULL, ls, &a) < 0)
		return -1;
	p->ts += 10;
	rp_tx(p, F_ACK, p->iss + 1, p->risn + 1, 0, 0, 0);
	sim_run_for(10 * MS);
	if (NS[0]->send(a, buf, 100, 0) != 100)
		return -1;
	return a;
}

/* H2: an orphan whose peer answers every window probe with a shut window
 * gives up once its backoff reached the maximal RTO (it used to probe for
 * ever, pinning its buffers and an orphan slot). */
static int t_zw_orphan_gives_up(void)
{
	struct rp p;
	struct tcpseg sg;
	int ls, a, probes = 0;
	simtime_t t0, last = 0;

	a = zw_setup(&p, 1, &ls);
	CHECK(a >= 0, "setup");
	NS[0]->close(a);
	t0 = now_ns();
	while (now_ns() - t0 < 1200 * SEC) {
		if (!rp_rx(&p, &sg, 150 * SEC))
			break;
		if (sg.flags & F_RST)
			break;
		probes++;
		last = now_ns();
		p.ts += 1000;
		rp_tx(&p, F_ACK, p.iss + 1, p.risn + 1, 0, 0, 0);
	}
	tlog("%d probes, the last after %.1f s", probes,
	     (double)(last - t0) / SEC);
	CHECK(n0_kt_tcp_orphans() == 0, "the orphan is still there after "
					 "%d probes",
	      probes);
	CHECK(probes >= 5 && probes <= 20, "%d probes", probes);
	CHECK(last - t0 < 400 * SEC, "probed for %.0f s",
	      (double)(last - t0) / SEC);
	CHECK(n0_kt_tcp_full_socks() == 1, "%u full sockets (the listener "
					   "only expected)",
	      n0_kt_tcp_full_socks());
	return 0;
}

/* H2 / RFC 1122 4.2.2.17: a connection the application still holds keeps
 * probing a shut window for as long as the peer answers. */
static int t_zw_alive_persists(void)
{
	struct rp p;
	struct tcpseg sg;
	int ls, a, probes = 0, data = 0;
	simtime_t t0;

	a = zw_setup(&p, 2, &ls);
	CHECK(a >= 0, "setup");
	t0 = now_ns();
	while (now_ns() - t0 < 900 * SEC) {
		CHECK(rp_rx(&p, &sg, 150 * SEC), "probing stopped after %d",
		      probes);
		probes++;
		p.ts += 1000;
		rp_tx(&p, F_ACK, p.iss + 1, p.risn + 1, 0, 0, 0);
	}
	CHECK(NS[0]->tcp_state(a) == TS_ESTABLISHED, "state %d after %d probes",
	      NS[0]->tcp_state(a), probes);
	// the window opens: the data goes
	p.ts += 1000;
	rp_tx(&p, F_ACK, p.iss + 1, p.risn + 1, 0, 65535, 0);
	while (!data && rp_rx(&p, &sg, 2 * SEC))
		data += (int)sg.len;
	CHECK(data == 100, "%d bytes after the window opened", data);
	return 0;
}

/* H2: TCP_USER_TIMEOUT counts from the first probe of the episode --
 * answers that keep the window shut are no progress. */
static int t_zw_user_timeout(void)
{
	struct rp p;
	struct tcpseg sg;
	int ls, a, probes = 0;
	simtime_t t0 = 0, tend = 0;

	a = zw_setup(&p, 3, &ls);
	CHECK(a >= 0, "setup");
	CHECK(setint(0, a, K_IPPROTO_TCP, K_TCP_USER_TIMEOUT, 10000) == 0,
	      "TCP_USER_TIMEOUT");
	while (!tend && now_ns() < 120 * SEC) {
		if (rp_rx(&p, &sg, 10 * MS)) {
			if (!t0)
				t0 = now_ns();
			probes++;
			p.ts += 100;
			rp_tx(&p, F_ACK, p.iss + 1, p.risn + 1, 0, 0, 0);
		}
		if (NS[0]->tcp_state(a) != TS_ESTABLISHED)
			tend = now_ns();
	}
	CHECK(tend, "still %d after %d probes", NS[0]->tcp_state(a), probes);
	tlog("%d probes, failed %.2f s after the first", probes,
	     (double)(tend - t0) / SEC);
	CHECK(tend - t0 >= 9 * SEC && tend - t0 <= 13 * SEC,
	      "failed after %.2f s", (double)(tend - t0) / SEC);
	CHECK(so_error(0, a) == ETIMEDOUT, "so_error %d", so_error(0, a));
	return 0;
}

/* H3: TCP over its global hard limit with in-order data nobody reads
 * (nothing the reclaim can take): a new connection's in-order data is
 * still accepted (the per-socket floor) -- it used to be dropped on every
 * connection of the host. */
#define NFLOOR 700
static int t_mem_floor(void)
{
	static struct rp p[NFLOOR + 1];
	struct tcpseg sg;
	int ls, k, got = 0;
	unsigned long long peak = 0;

	ls = NS[0]->socket(K_SOCK_STREAM);
	NS[0]->set_nonblock(ls, 1);
	CHECK(NS[0]->bind(ls, 0, 8080) == 0 && NS[0]->listen(ls, 4096) == 0,
	      "listen");
	for (k = 0; k <= NFLOOR; k++) {
		cap_start(0, 1);
		rp_init(&p[k], 0, (uint16_t)(20000 + k), 8080);
		CHECK(rp_connect_rtt(&p[k], MS, 0, NULL) == 0, "handshake %d", k);
	}
	sim_run_for(10 * MS);
	// every connection but the last fills its window, nobody reads
	for (k = 0; k < NFLOOR && n0_kt_tcp_mem() <= (48ull << 20); k++) {
		uint32_t off = 0;

		cap_start(0, 1);
		for (int j = 0; j < 48; j++) {
			p[k].ts++;
			rp_tx(&p[k], F_ACK | F_PSH, p[k].iss + 1 + off,
			      p[k].risn + 1, 1400, 65535, 0);
			off += 1400;
		}
		if (n0_kt_tcp_mem() > peak)
			peak = n0_kt_tcp_mem();
	}
	tlog("%d connections hold %llu bytes (peak %llu)", k, n0_kt_tcp_mem(),
	     peak);
	CHECK(n0_kt_tcp_mem() > (48ull << 20), "the limit was not reached: "
					       "%llu",
	      n0_kt_tcp_mem());
	// the last connection: its in-order data is taken
	cap_start(0, 1);
	p[NFLOOR].ts++;
	rp_tx(&p[NFLOOR], F_ACK | F_PSH, p[NFLOOR].iss + 1,
	      p[NFLOOR].risn + 1, 1000, 65535, 0);
	while (rp_rx(&p[NFLOOR], &sg, 300 * MS))
		if (sg.ack == p[NFLOOR].iss + 1001)
			got = 1;
	CHECK(got, "in-order data refused over the limit (RcvQDrop %llu)",
	      xmib(0, "TCPRcvQDrop"));
	return 0;
}

/* H3: peers parking out-of-order data (a hole never filled) push TCP over
 * its hard limit: the reclaim takes everybody's out-of-order queues and
 * brings the total below the pressure mark; the hole then fills. */
#define NPARK 420
static int t_mem_ooo_reclaim(void)
{
	static struct rp p[NPARK];
	struct tcpseg sg;
	int ls, k, got = 0;
	unsigned long long r0 = xmib(0, "TCPMemoryReclaims"), peak = 0;

	ls = NS[0]->socket(K_SOCK_STREAM);
	NS[0]->set_nonblock(ls, 1);
	CHECK(NS[0]->bind(ls, 0, 8080) == 0 && NS[0]->listen(ls, 4096) == 0,
	      "listen");
	for (k = 0; k < NPARK; k++) {
		cap_start(0, 1);
		rp_init(&p[k], 0, (uint16_t)(20000 + k), 8080);
		CHECK(rp_connect_rtt(&p[k], MS, 0, NULL) == 0, "handshake %d", k);
	}
	sim_run_for(10 * MS);
	for (k = 0; k < NPARK; k++) {
		cap_start(0, 1);
		/* one byte missing at the front, then small segments --
		 * just below the receive buffer, so the connection itself
		 * never prunes */
		for (int j = 0; j < 66; j++) {
			p[k].ts++;
			rp_tx(&p[k], F_ACK, p[k].iss + 2 + (uint32_t)j * 100,
			      p[k].risn + 1, 100, 65535, 0);
			if (n0_kt_tcp_mem() > peak)
				peak = n0_kt_tcp_mem();
		}
	}
	sim_run_for(300 * MS);
	tlog("peak %llu bytes, now %llu; %llu reclaim passes, %llu prunes",
	     peak, n0_kt_tcp_mem(), xmib(0, "TCPMemoryReclaims") - r0,
	     xmib(0, "OfoPruned"));
	CHECK(xmib(0, "TCPMemoryReclaims") > r0, "no reclaim");
	CHECK(peak <= (49ull << 20), "the total went to %llu", peak);
	/* Below the hard limit again (the pass stops below the pressure
	 * mark; what was parked after it stays below the limit). */
	CHECK(n0_kt_tcp_mem() < (48ull << 20), "still %llu after the reclaim",
	      n0_kt_tcp_mem());
	// the first connection's hole fills: everything it holds is ACKed
	cap_start(0, 1);
	p[0].ts++;
	rp_tx(&p[0], F_ACK, p[0].iss + 1, p[0].risn + 1, 1, 65535, 0);
	while (rp_rx(&p[0], &sg, 300 * MS))
		if ((int32_t)(sg.ack - (p[0].iss + 1)) > 0)
			got = 1;
	CHECK(got, "the in-order byte was refused");
	return 0;
}

/* H1 (the published shape; the race itself needs two CPUs): a TIME_WAIT
 * sock is hashed, bound and timed with one reference each; a child
 * waiting on the accept queue is bound. */
static int t_published_shape(void)
{
	struct rp p;
	struct tcpseg sg;
	int ls, a;

	CHECK(rp_server(&p, 5, 10 * MS, 0, NULL, &ls, &a) == 0, "setup");
	CHECK(n0_kt3_full_published(inst_ip(0), 8080, p.ip, p.port) == 0,
	      "accepted child: %s", n0_kt_msg);
	// a second child, not accepted yet
	{
		struct rp q;

		rp_init(&q, 6, 34006, 8080);
		CHECK(rp_connect_rtt(&q, 10 * MS, 0, NULL) == 0, "handshake");
		sim_run_for(2 * MS);
		CHECK(n0_kt3_full_published(inst_ip(0), 8080, q.ip, q.port) ==
			      0,
		      "queued child: %s", n0_kt_msg);
	}
	// the peer closes, then we do: TIME_WAIT here
	p.ts += 10;
	rp_tx(&p, F_ACK | F_FIN, p.iss + 1, p.risn + 1, 0, 65535, 0);
	sim_run_for(50 * MS);
	NS[0]->shutdown(a, K_SHUT_WR);
	sg.flags = 0;
	while (!(sg.flags & F_FIN) && rp_rx(&p, &sg, 300 * MS))
		;
	CHECK(sg.flags & F_FIN, "no FIN");
	p.ts += 10;
	rp_tx(&p, F_ACK, p.iss + 2, p.risn + 2, 0, 65535, 0);
	sim_run_for(10 * MS);
	CHECK(n0_kt_tcp_tw() == 0, "TIME_WAIT on the passive closer");
	// active close the other way: our FIN first, then theirs
	{
		struct rp q;
		int b, mine = -1;

		rp_init(&q, 7, 34007, 8080);
		CHECK(rp_connect_rtt(&q, 10 * MS, 0, NULL) == 0, "handshake 2");
		sim_run_for(2 * MS);
		while ((b = NS[0]->accept(ls, NULL, NULL)) >= 0) {
			unsigned ip, port;

			NS[0]->set_nonblock(b, 1);
			if (NS[0]->sockname(b, 1, &ip, &port) == 0 &&
			    port == q.port)
				mine = b;
		}
		CHECK(mine >= 0, "the child was not accepted");
		CHECK(n0_kt3_full_published(inst_ip(0), 8080, q.ip, q.port) ==
			      0,
		      "child 2: %s", n0_kt_msg);
		cap_start(0, 1);
		NS[0]->shutdown(mine, K_SHUT_WR);
		sg.flags = 0;
		while (!(sg.flags & F_FIN) && rp_rx(&q, &sg, 300 * MS))
			;
		CHECK(sg.flags & F_FIN, "no FIN from us");
		q.ts += 10;
		rp_tx(&q, F_ACK | F_FIN, q.iss + 1, q.risn + 2, 0, 65535, 0);
		sim_run_for(10 * MS);
		CHECK(n0_kt_tcp_tw() >= 1, "no TIME_WAIT");
		CHECK(n0_kt3_tw_published(inst_ip(0), 8080, q.ip, q.port) == 0,
		      "TIME_WAIT: %s", n0_kt_msg);
	}
	return 0;
}

/* L4: the TIME_WAIT reuse policy (tcp_tw_reuse=2). */
static int t_tw_reuse_policy(void)
{
	sim_run_for(5 * SEC);
	CHECK(n0_kt3_twsk_unique_policy() == 0, "%s", n0_kt_msg);
	return 0;
}

/* L2: under random loss and reordering with SACK, the time-ordered list
 * of the flight and the sequence index stay consistent all the way. */
static int t_flight_structures(void)
{
	struct path_cfg pc = { 0 };
	struct flow f;
	int c, s, bad = 0;
	unsigned maxl = 0, maxi = 0;

	arp_warm();
	pc.delay_ms = 20;
	pc.loss = 0.02;
	pc.reorder = 0.05;
	pc.reorder_ms = 10;
	link_set_both(&pc);
	link_cfg(0)->rate_mbps = link_cfg(1)->rate_mbps = 200;
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 30 * SEC) == 0, "pair");
	flow_init(&f, 1, s, 0, c, 41, 30ULL * 1000 * 1000);
	for (int i = 0; i < 2000 && !bad; i++) {
		unsigned nl, ni;

		flows_run(&f, 1, 10 * MS);
		if (n1_kt3_flight_check(s, &nl, &ni)) {
			tlog("check %d: %s", i, n1_kt_msg);
			bad = 1;
		}
		if (nl > maxl)
			maxl = nl;
		if (ni > maxi)
			maxi = ni;
		if (f.got >= f.n)
			break;
	}
	tlog("flight list up to %u, index up to %u entries; %llu bytes", maxl,
	     maxi, f.got);
	CHECK(!bad, "flight structures broken");
	CHECK(maxl > 10 && maxi > 2, "nothing was checked (%u, %u)", maxl,
	      maxi);
	return 0;
}

/* L2: a receiver that SACKs everything but the head: RACK finds the head
 * lost from the time-ordered list, it is resent, the data completes. */
static int t_sack_all_but_head(void)
{
	struct rp p;
	struct tcpseg v[64];
	static uint8_t buf[60000];
	int ls, a, n, head_resent = 0;
	uint32_t head, high;

	CHECK(rp_server(&p, 8, 20 * MS, 0, NULL, &ls, &a) == 0, "setup");
	p.ts += 10;
	rp_tx(&p, F_ACK, p.iss + 1, p.risn + 1, 0, 65535, 0);
	CHECK(NS[0]->send(a, buf, 14000, 0) == 14000, "send");
	n = rp_collect(&p, v, 64, 30 * MS);
	CHECK(n >= 5, "%d segments", n);
	head = v[0].seq;
	high = v[n - 1].seq + v[n - 1].len;
	// SACK everything but the head, one segment more per ACK
	for (int i = 1; i < n; i++) {
		uint32_t blk[2] = { v[1].seq, v[i].seq + v[i].len };

		p.ts++;
		rp_send(&p, F_ACK, p.iss + 1, head, 0, 65535, 0, 1, blk);
		{
			struct tcpseg sg;

			while (rp_rx(&p, &sg, 0))
				if (sg.len && sg.seq == head)
					head_resent = 1;
		}
	}
	{
		struct tcpseg sg;

		while (!head_resent && rp_rx(&p, &sg, 2 * SEC))
			if (sg.len && sg.seq == head)
				head_resent = 1;
	}
	CHECK(head_resent, "the head was never resent");
	p.ts++;
	rp_tx(&p, F_ACK, p.iss + 1, high, 0, 65535, 0);
	sim_run_for(50 * MS);
	{
		unsigned nl, ni;

		CHECK(n0_kt3_flight_check(a, &nl, &ni) == 0, "%s", n0_kt_msg);
		CHECK(nl == 0, "%u segments left on the flight list", nl);
	}
	return 0;
}

/* L8: an ACK of data never sent, outside ESTABLISHED, is answered with
 * one challenge ACK (it used to be two, the second rate limited). */
static int t_single_challenge_ack(void)
{
	struct rp p;
	struct tcpseg sg;
	int ls, a, acks = 0;
	unsigned long long sk0;

	CHECK(rp_server(&p, 9, 10 * MS, 0, NULL, &ls, &a) == 0, "setup");
	p.ts += 10;
	rp_tx(&p, F_ACK | F_FIN, p.iss + 1, p.risn + 1, 0, 65535, 0);
	CHECK(rp_rx(&p, &sg, 300 * MS) && sg.ack == p.iss + 2, "FIN not ACKed");
	CHECK(NS[0]->tcp_state(a) == TS_CLOSE_WAIT, "state %d",
	      NS[0]->tcp_state(a));
	sim_run_for(600 * MS);
	sk0 = xmib(0, "TCPACKSkippedChallenge");
	p.ts += 10;
	rp_tx(&p, F_ACK, p.iss + 2, p.risn + 5000, 0, 65535, 0);
	while (rp_rx(&p, &sg, 100 * MS))
		acks++;
	CHECK(acks == 1, "%d ACKs", acks);
	CHECK(xmib(0, "TCPACKSkippedChallenge") == sk0,
	      "a second challenge ACK was attempted");
	return 0;
}

/* M3: socket options on a socket whose connection failed (detached) and
 * on one connected: they read and write the right object, no crash. */
static int t_sockopt_detached(void)
{
	int c = NS[0]->socket(K_SOCK_STREAM), v = 0, r;
	unsigned len = sizeof(v);
	struct abi_tcp_info ti;

	NS[0]->set_nonblock(c, 1);
	r = NS[0]->connect(c, inst_ip(1), 9); // nobody listens there
	CHECK(r == -EINPROGRESS || r == -ECONNREFUSED, "connect %d", r);
	sim_run_for(100 * MS);
	CHECK(so_error(0, c) == ECONNREFUSED, "so_error");
	CHECK(so_error(0, c) == 0, "so_error reported twice");
	r = NS[0]->connect(c, inst_ip(1), 9);
	CHECK(r < 0, "second connect %d", r);
	CHECK(setint(0, c, K_IPPROTO_TCP, 4 /* TCP_KEEPIDLE */, 30) == 0,
	      "KEEPIDLE");
	CHECK(NS[0]->getsockopt(c, K_IPPROTO_TCP, 4, &v, &len) == 0 && v == 30,
	      "KEEPIDLE reads %d", v);
	CHECK(setint(0, c, K_IPPROTO_TCP, K_TCP_NODELAY, 0) == 0, "NODELAY");
	CHECK(tinfo(0, c, &ti) == -ENOTCONN || ti.state == 7, "TCP_INFO");
	// on a live connection the options reach it
	{
		int x, y;

		CHECK(tcp_pair(0, 1, 8081, &x, &y, NULL, 5 * SEC) == 0, "pair");
		CHECK(setint(0, x, K_IPPROTO_TCP, 4, 77) == 0, "KEEPIDLE live");
		v = 0;
		len = sizeof(v);
		CHECK(NS[0]->getsockopt(x, K_IPPROTO_TCP, 4, &v, &len) == 0 &&
			      v == 77,
		      "live KEEPIDLE reads %d", v);
		CHECK(tinfo(0, x, &ti) == 0 && ti.state == 1, "TCP_INFO live");
	}
	return 0;
}

/* Pacing: off by default (segments leave as the windows allow); a socket
 * that sets SO_MAX_PACING_RATE is paced, at most at that rate. */
static int t_pacing_default_and_cap(void)
{
	struct path_cfg pc = { 0 };
	struct flow f;
	struct abi_tcp_info ti;
	int c, s;
	unsigned long long d0, cap = 1000000; // 8 Mbit/s
	unsigned long long got0;

	pc.delay_ms = 25;
	link_set_both(&pc);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	flow_init(&f, 1, s, 0, c, 12, 20ULL * 1000 * 1000);
	d0 = xmib(1, "TCPPacingDefer");
	flows_run(&f, 1, 600 * MS);
	CHECK(xmib(1, "TCPPacingDefer") == d0, "paced without being asked");
	CHECK(tinfo(1, s, &ti) == 0 && ti.pacing_rate > 0 &&
		      ti.max_pacing_rate == ~0ULL,
	      "TCP_INFO pacing %llu max %llu",
	      (unsigned long long)ti.pacing_rate,
	      (unsigned long long)ti.max_pacing_rate);
	// capped: the rate and the throughput follow the cap
	CHECK(NS[1]->setsockopt(s, K_SOL_SOCKET, K_SO_MAX_PACING_RATE, &cap,
				sizeof(cap)) == 0,
	      "SO_MAX_PACING_RATE");
	flows_run(&f, 1, 500 * MS);
	got0 = f.got;
	flows_run(&f, 1, 2 * SEC);
	CHECK(tinfo(1, s, &ti) == 0 && ti.pacing_rate <= cap &&
		      ti.max_pacing_rate == cap,
	      "pacing %llu max %llu", (unsigned long long)ti.pacing_rate,
	      (unsigned long long)ti.max_pacing_rate);
	tlog("capped at %llu B/s: %.0f B/s over 2 s", cap,
	     (double)(f.got - got0) / 2.0);
	CHECK(f.got - got0 <= cap * 2 * 105 / 100 &&
		      f.got - got0 >= cap * 2 * 25 / 100,
	      "%llu bytes in 2 s at a cap of %llu B/s", f.got - got0, cap);
	CHECK(xmib(1, "TCPPacingDefer") > d0, "nothing was paced");
	{
		unsigned long long v = 0;
		unsigned len = sizeof(v);

		CHECK(NS[1]->getsockopt(s, K_SOL_SOCKET, K_SO_MAX_PACING_RATE,
					&v, &len) == 0 &&
			      v == cap && len == 8,
		      "reads back %llu", v);
	}
	return 0;
}

/* L1: a small receive buffer nobody reads holds in-order data, and tiny
 * out-of-order segments keep arriving behind a hole: every one of them
 * prunes, but the receive queue is collapsed once -- its compact copies
 * are not copied again for every segment. */
static int t_collapse_once(void)
{
	struct rp p;
	struct tcpseg sg;
	int ls, a, rb = 16384, got;
	struct kt_tcp_sockinfo si;
	unsigned long long c0, c1, c2, pr1, pr2;
	static uint8_t buf[4096];

	ls = NS[0]->socket(K_SOCK_STREAM);
	NS[0]->set_nonblock(ls, 1);
	CHECK(setint(0, ls, K_SOL_SOCKET, K_SO_RCVBUF, rb) == 0, "SO_RCVBUF");
	CHECK(NS[0]->bind(ls, 0, 8080) == 0 && NS[0]->listen(ls, 16) == 0,
	      "listen");
	cap_start(0, 1);
	rp_init(&p, 10, 34010, 8080);
	CHECK(rp_connect_rtt(&p, 10 * MS, 0, NULL) == 0, "handshake");
	sim_run_for(2 * MS);
	a = NS[0]->accept(ls, NULL, NULL);
	CHECK(a >= 0, "accept");
	NS[0]->set_nonblock(a, 1);
	c0 = xmib(0, "TCPRcvCollapsed");
	/* 4 full segments (they teach the receiver its payload:truesize
	 * ratio), then 3000 bytes in order, 100 at a time: those buffers
	 * are mostly empty -- collapsed into compact ones */
	for (int j = 0; j < 4; j++) {
		p.ts++;
		rp_tx(&p, F_ACK | F_PSH, p.iss + 1 + (uint32_t)j * 1400,
		      p.risn + 1, 1400, 65535, 0);
		while (rp_rx(&p, &sg, 0))
			;
	}
	for (int j = 0; j < 30; j++) {
		p.ts++;
		rp_tx(&p, F_ACK | F_PSH, p.iss + 5601 + (uint32_t)j * 100,
		      p.risn + 1, 100, 65535, 0);
		while (rp_rx(&p, &sg, 0))
			;
	}
	c1 = xmib(0, "TCPRcvCollapsed");
	pr1 = xmib(0, "PruneCalled");
	n0_kt_tcp_sockinfo(a, &si);
	tlog("rcvbuf %d rmem %u, %u skbs, %u bytes queued, ratio %u",
	     si.rcvbuf, si.rmem_alloc, si.rcvq_skbs, si.rcvq_bytes,
	     si.scaling_ratio);
	// tiny segments out of order, behind a one-byte hole
	for (int j = 0; j < 40; j++) {
		p.ts++;
		rp_tx(&p, F_ACK, p.iss + 8601 + 1 + (uint32_t)j * 10,
		      p.risn + 1, 10, 65535, 0);
		while (rp_rx(&p, &sg, 0))
			;
	}
	c2 = xmib(0, "TCPRcvCollapsed");
	pr2 = xmib(0, "PruneCalled");
	tlog("collapsed buffers: %llu for the in-order data, %llu during %llu "
	     "prunes after",
	     c1 - c0, c2 - c1, pr2 - pr1);
	CHECK(c1 > c0, "never collapsed");
	CHECK(pr2 - pr1 >= 5, "only %llu prunes", pr2 - pr1);
	CHECK(c2 - c1 <= 2, "collapsed %llu buffers again during %llu prunes",
	      c2 - c1, pr2 - pr1);
	got = recv_all(0, a, buf, 4000, SEC, 77, 0);
	CHECK(got == 4000, "read %d of 4000 (-2: corrupt)", got);
	return 0;
}

#define T(n, f, ...) { .name = n, .group = "unit", .fn = f, __VA_ARGS__ }
const struct test tests_tcp2[] = {
	T("tcp-sacktag", t_sacktag, .ninst = 1),
	T("tcp-sack-reneging", t_sack_reneging, .ninst = 1),
	T("tcp-tlp-tail", t_tlp_recovers_tail),
	T("tcp-rack-reorder", t_rack_reorder),
	T("tcp-prr-counts", t_prr_counts, .ninst = 1),
	T("tcp-frto-spurious", t_frto_spurious, .ninst = 1),
	T("tcp-eifel-undo", t_eifel_undo, .ninst = 1),
	T("tcp-cubic-root", t_cubic_root, .ninst = 1),
	T("tcp-cubic-curve", t_cubic_curve, .ninst = 1),
	T("tcp-hystart-exit", t_hystart_exit),
	T("tcp-pacing", t_pacing),
	T("tcp-ecn", t_ecn, .ninst = 1),
	T("tcp-ecn-not-requested", t_ecn_not_requested, .ninst = 1),
	T("tcp-pmtu-midstream", t_pmtu_midstream),
	T("tcp-gro-segments", t_gro_segments, .ninst = 1),
	T("tcp-info-fields", t_tcp_info_fields),
	T("tcp-congestion", t_tcp_congestion),
	T("tcp-tw-eaddrnotavail", t_tw_eaddrnotavail, .ninst = 1),
	T("tcp-loss1-no-rto", t_loss1_no_rto),
	T("tcp-multi-loss-one-recovery", t_multi_loss_one_recovery),
	T("tcp-gro-truesize", t_gro_truesize),
	T("tcp-zw-orphan-gives-up", t_zw_orphan_gives_up, .ninst = 1),
	T("tcp-zw-alive-persists", t_zw_alive_persists, .ninst = 1),
	T("tcp-zw-user-timeout", t_zw_user_timeout, .ninst = 1),
	T("tcp-mem-floor", t_mem_floor, .ninst = 1),
	T("tcp-mem-ooo-reclaim", t_mem_ooo_reclaim, .ninst = 1),
	T("tcp-published-shape", t_published_shape, .ninst = 1),
	T("tcp-tw-reuse-policy", t_tw_reuse_policy, .ninst = 1),
	T("tcp-flight-structures", t_flight_structures),
	T("tcp-sack-all-but-head", t_sack_all_but_head, .ninst = 1),
	T("tcp-single-challenge-ack", t_single_challenge_ack, .ninst = 1),
	T("tcp-sockopt-detached", t_sockopt_detached),
	T("tcp-pacing-default-and-cap", t_pacing_default_and_cap),
	T("tcp-collapse-once", t_collapse_once, .ninst = 1),
	{ 0 },
};
