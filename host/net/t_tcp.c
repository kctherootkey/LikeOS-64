/* TCP tests of the network harness (wave 5: request socks, TIME_WAIT
 * socks, skb queues, memory accounting and auto-tuning, delayed ACKs,
 * window updates, Nagle/cork, PSH placement, SYN cookies, options).
 *
 * Two kinds: interop between the two stack instances over the simulated
 * wire (often with a tap that timestamps what one side sends), and a
 * "raw peer" -- a made-up host whose segments are crafted here and fed to
 * instance 0, everything instance 0 sends captured.  The instances'
 * internal state is read through the probes of host/net/kt_tcp.c.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "tests.h"
#include "pkt.h"
#include "kt_tcp.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* kernel-side probes (host/net/kt_tcp.c), per instance */
extern char n0_kt_msg[256];
int n0_kt_tcp_sockinfo(int sock, struct kt_tcp_sockinfo *o);
int n1_kt_tcp_sockinfo(int sock, struct kt_tcp_sockinfo *o);
unsigned long long n0_kt_tcp_xmib(const char *name);
unsigned long long n1_kt_tcp_xmib(const char *name);
unsigned n0_kt_tcp_full_socks(void);
unsigned n1_kt_tcp_full_socks(void);
unsigned n0_kt_tcp_tw(void);
unsigned n1_kt_tcp_tw(void);
unsigned long long n0_kt_tcp_mem(void);
unsigned long long n1_kt_tcp_mem(void);
int n0_kt_rbtree(unsigned seed, int nodes, int ops);

static int sockinfo(int inst, int s, struct kt_tcp_sockinfo *o)
{
	return inst ? n1_kt_tcp_sockinfo(s, o) : n0_kt_tcp_sockinfo(s, o);
}

static unsigned long long xmib(int inst, const char *name)
{
	return inst ? n1_kt_tcp_xmib(name) : n0_kt_tcp_xmib(name);
}

static unsigned full_socks(int inst)
{
	return inst ? n1_kt_tcp_full_socks() : n0_kt_tcp_full_socks();
}

#define K_TCP_MAXSEG 2
#define K_TCP_KEEPIDLE 4
#define K_TCP_KEEPINTVL 5
#define K_TCP_KEEPCNT 6
#define K_TCP_DEFER_ACCEPT 9
#define K_TCP_QUICKACK 12
#define K_TCP_USER_TIMEOUT 18
#define K_TCP_NOTSENT_LOWAT 25
#define K_SO_REUSEADDR 2

static int setint(int inst, int s, int level, int name, int v)
{
	return NS[inst]->setsockopt(s, level, name, &v, sizeof(v));
}

static void my_mac(int i, uint8_t m[6])
{
	unsigned char t[6];

	inst_mac(i, t);
	memcpy(m, t, 6);
}

/* ======================================================================
 * The raw peer: a made-up host 10.0.0.(20+k) talking to instance 0.
 * ====================================================================== */
struct rp {
	uint32_t ip;
	uint8_t mac[6];
	uint16_t port, lport;
	uint32_t iss; // ours
	uint32_t risn; // instance 0's
	uint32_t ts; // our TSval clock
	uint32_t tsecr; // instance 0's last TSval
	int use_ts;
};

static void rp_init(struct rp *p, int k, uint16_t port, uint16_t lport)
{
	memset(p, 0, sizeof(*p));
	p->ip = (10u << 24) | (uint32_t)(20 + k);
	p->mac[0] = 0x02;
	p->mac[5] = (uint8_t)(0x20 + k);
	p->port = port;
	p->lport = lport;
	p->iss = 1000000u + (uint32_t)k * 7919u;
	p->ts = 1000;
	p->use_ts = 1;
	NS[0]->arp_add(p->ip, p->mac);
}

/* Send a segment; data is pattern 77 at its stream offset. */
static void rp_tx(struct rp *p, uint8_t flags, uint32_t seq, uint32_t ack,
		  unsigned len, uint16_t win, int synopts)
{
	uint8_t f[1700], data[1500], m0[6];
	struct tcpseg s;
	unsigned n;

	my_mac(0, m0);
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
	if (len > sizeof(data))
		len = sizeof(data);
	pat_fill(77, seq - (p->iss + 1), data, len);
	s.data = data;
	s.len = len;
	n = pkt_tcp(f, m0, p->mac, &s);
	NS[0]->rx(f, n);
}

/* The next segment instance 0 sends this peer; 0 when none came. */
static int rp_rx(struct rp *p, struct tcpseg *sg, uint8_t *f, simtime_t wait)
{
	simtime_t end = now_ns() + wait;

	for (;;) {
		simtime_t left = end > now_ns() ? end - now_ns() : 0;
		unsigned n = cap_next_proto(6, f, 1700, left);

		if (!n)
			return 0;
		if (pkt_parse_tcp(f, n, sg) == 0 && sg->dip == p->ip &&
		    sg->dport == p->port) {
			if (sg->ts)
				p->tsecr = sg->tsval;
			return 1;
		}
		if (!left)
			return 0;
	}
}

/* Handshake toward a listener on instance 0: SYN, SYN-ACK, ACK. */
static int rp_connect(struct rp *p)
{
	struct tcpseg sg;
	uint8_t f[1700];

	rp_tx(p, F_SYN, p->iss, 0, 0, 65535, 1);
	if (!rp_rx(p, &sg, f, 100 * MS) || sg.flags != (F_SYN | F_ACK))
		return -1;
	p->risn = sg.seq;
	p->ts += 10;
	rp_tx(p, F_ACK, p->iss + 1, p->risn + 1, 0, 512, 0);
	return 0;
}

/* ======================================================================
 * Taps
 * ====================================================================== */
#define TAPMAX 20000
static struct tapseg {
	int inst;
	simtime_t t;
	uint32_t seq, ack;
	uint8_t flags;
	unsigned len, win;
	int nsack;
	uint32_t sack0l, sack0r;
	int mss, ws;
} tapv[TAPMAX];
static int tapn;
static int tap_drop_inst = -1; // drop everything this instance sends

static int seg_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	struct tcpseg s;

	(void)ctx;
	if (pkt_parse_tcp(f, len, &s) == 0 && tapn < TAPMAX) {
		struct tapseg *t = &tapv[tapn++];

		t->inst = inst;
		t->t = now_ns();
		t->seq = s.seq;
		t->ack = s.ack;
		t->flags = s.flags;
		t->len = s.len;
		t->win = s.win;
		t->nsack = s.nsack;
		t->sack0l = s.nsack ? s.sack[0][0] : 0;
		t->sack0r = s.nsack ? s.sack[0][1] : 0;
		t->mss = s.mss;
		t->ws = s.ws;
	}
	return inst == tap_drop_inst;
}

static void tap_on(void)
{
	tapn = 0;
	tap_drop_inst = -1;
	wire_set_tap(seg_tap, NULL);
}

static void tap_off(void)
{
	wire_set_tap(NULL, NULL);
	tap_drop_inst = -1;
}

/* ======================================================================
 * Data structures
 * ====================================================================== */
static int t_rbtree(void)
{
	for (unsigned seed = 1; seed <= 6; seed++) {
		int r = n0_kt_rbtree(seed, 50 + (int)seed * 40, 20000);

		CHECK(r == 0, "line %d: %s", r, n0_kt_msg);
	}
	return 0;
}

/* ======================================================================
 * The listen path: request socks, SYN-ACK retransmission, cookies
 * ====================================================================== */

/* A SYN makes a request sock, not a socket: no buffers, no full socket,
 * until the handshake completes. */
static int t_reqsk_small(void)
{
	enum { N = 100 };
	struct rp p[N];
	struct tcpseg sg;
	uint8_t f[1700];
	int ls = NS[0]->socket(K_SOCK_STREAM);

	NS[0]->set_nonblock(ls, 1);
	CHECK(NS[0]->bind(ls, 0, 8080) == 0 && NS[0]->listen(ls, 256) == 0,
	      "listen");
	cap_start(0, 1);
	unsigned full0 = full_socks(0);
	unsigned long long heap0 = NS[0]->counter(NSC_KALLOC_BYTES);
	for (int i = 0; i < N; i++) {
		rp_init(&p[i], i % 40, (uint16_t)(30000 + i), 8080);
		rp_tx(&p[i], F_SYN, p[i].iss, 0, 0, 65535, 1);
		CHECK(rp_rx(&p[i], &sg, f, 50 * MS) &&
			      sg.flags == (F_SYN | F_ACK) &&
			      sg.ack == p[i].iss + 1,
		      "no SYN-ACK for %d", i);
		p[i].risn = sg.seq;
	}
	unsigned long long heap1 = NS[0]->counter(NSC_KALLOC_BYTES);
	CHECK(full_socks(0) == full0, "a SYN allocated a full socket (%u -> %u)",
	      full0, full_socks(0));
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 1 + N, "%llu table entries",
	      NS[0]->counter(NSC_TCP_CONNS));
	tlog("request sock: %llu bytes each", (heap1 - heap0) / N);
	CHECK(heap1 - heap0 < N * 512ull, "%llu heap bytes for %d SYNs",
	      heap1 - heap0, N);
	// complete half of them: they become sockets and are accepted
	for (int i = 0; i < N / 2; i++)
		rp_tx(&p[i], F_ACK, p[i].iss + 1, p[i].risn + 1, 0, 512, 0);
	sim_run_for(10 * MS);
	int got = 0;
	while (NS[0]->accept(ls, NULL, NULL) >= 0)
		got++;
	CHECK(got == N / 2, "accepted %d of %d", got, N / 2);
	CHECK(full_socks(0) == full0 + N / 2, "full sockets %u",
	      full_socks(0));
	return 0;
}

/* SYN-ACK retransmissions back off from 1 s, five of them, then the
 * request sock is gone. */
static int t_synack_backoff(void)
{
	struct rp p;
	struct tcpseg sg;
	uint8_t f[1700];
	simtime_t t[7];
	int ls = NS[0]->socket(K_SOCK_STREAM);
	int n = 0;

	NS[0]->set_nonblock(ls, 1);
	CHECK(NS[0]->bind(ls, 0, 8080) == 0 && NS[0]->listen(ls, 16) == 0,
	      "listen");
	cap_start(0, 1);
	rp_init(&p, 1, 40000, 8080);
	rp_tx(&p, F_SYN, p.iss, 0, 0, 65535, 1);
	while (n < 7 && rp_rx(&p, &sg, f, 40 * SEC)) {
		if (sg.flags == (F_SYN | F_ACK))
			t[n++] = now_ns();
	}
	CHECK(n == 6, "%d SYN-ACKs (1 + 5 retransmissions expected)", n);
	for (int i = 1; i < n; i++) {
		double dt = (double)(t[i] - t[i - 1]) / 1e9;
		double want = (double)(1u << (i - 1));

		CHECK(dt > want - 0.02 && dt < want + 0.02,
		      "SYN-ACK %d after %.3f s, expected %.0f s", i, dt, want);
	}
	sim_run_for(40 * SEC);
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 1,
	      "request sock still there (%llu entries)",
	      NS[0]->counter(NSC_TCP_CONNS));
	return 0;
}

/* A full SYN queue answers with cookies; the cookie's ACK makes a
 * connection with the options the SYN asked for. */
static int t_syncookie(void)
{
	struct rp p[6];
	struct tcpseg sg;
	uint8_t f[1700];
	struct kt_tcp_sockinfo si;
	int ls = NS[0]->socket(K_SOCK_STREAM);

	NS[0]->set_nonblock(ls, 1);
	CHECK(NS[0]->bind(ls, 0, 8080) == 0 && NS[0]->listen(ls, 4) == 0,
	      "listen");
	cap_start(0, 1);
	for (int i = 0; i < 6; i++) {
		rp_init(&p[i], i, (uint16_t)(31000 + i), 8080);
		rp_tx(&p[i], F_SYN, p[i].iss, 0, 0, 65535, 1);
		CHECK(rp_rx(&p[i], &sg, f, 50 * MS) &&
			      sg.flags == (F_SYN | F_ACK),
		      "no SYN-ACK %d", i);
		p[i].risn = sg.seq;
		if (i >= 4)
			CHECK(sg.ws == 7 && sg.ts && sg.mss > 0,
			      "cookie SYN-ACK options ws %d ts %d mss %d",
			      sg.ws, sg.ts, sg.mss);
	}
	CHECK(NS[0]->counter(NSC_TCP_SYNCOOKIE_SENT) >= 2,
	      "%llu cookies sent", NS[0]->counter(NSC_TCP_SYNCOOKIE_SENT));
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 5, "%llu entries (listener + 4)",
	      NS[0]->counter(NSC_TCP_CONNS));
	// a forged cookie gets a reset
	{
		struct rp bad = p[5];

		bad.ts += 5;
		rp_tx(&bad, F_ACK, bad.iss + 1, bad.risn + 77, 0, 512, 0);
		CHECK(rp_rx(&bad, &sg, f, 50 * MS) && (sg.flags & F_RST),
		      "no RST for a bad cookie (flags %x)", sg.flags);
	}
	// the real one connects
	p[4].ts += 5;
	rp_tx(&p[4], F_ACK, p[4].iss + 1, p[4].risn + 1, 0, 512, 0);
	sim_run_for(5 * MS);
	int a = NS[0]->accept(ls, NULL, NULL);
	CHECK(a >= 0, "cookie connection not accepted: %d", a);
	CHECK(sockinfo(0, a, &si) == 0 && si.state == TS_ESTABLISHED,
	      "state %d", si.state);
	CHECK(si.ws_enabled && si.snd_wscale == 7 && si.sack_ok &&
		      si.ts_enabled,
	      "cookie lost options: ws %u/%u sack %u ts %u", si.ws_enabled,
	      si.snd_wscale, si.sack_ok, si.ts_enabled);
	// data both ways on it
	p[4].ts += 5;
	rp_tx(&p[4], F_ACK | F_PSH, p[4].iss + 1, p[4].risn + 1, 300, 512, 0);
	sim_run_for(5 * MS);
	uint8_t buf[400];
	int r = NS[0]->recv(a, buf, sizeof(buf), 0);
	CHECK(r == 300 && !pat_check(77, 0, buf, 300), "recv %d", r);
	return 0;
}

/* Many clients complete the handshake while the SYN queue overflows:
 * every one of them gets a connection (request sock or cookie). */
static int t_synflood_valid_acks(void)
{
	enum { N = 300 };
	static struct rp p[N];
	struct tcpseg sg;
	uint8_t f[1700];
	int ls = NS[0]->socket(K_SOCK_STREAM), got = 0;

	NS[0]->set_nonblock(ls, 1);
	CHECK(NS[0]->bind(ls, 0, 8080) == 0 && NS[0]->listen(ls, 16) == 0,
	      "listen");
	cap_start(0, 1);
	for (int i = 0; i < N; i++) {
		rp_init(&p[i], i % 50, (uint16_t)(32000 + i), 8080);
		rp_tx(&p[i], F_SYN, p[i].iss, 0, 0, 65535, 1);
	}
	for (int i = 0; i < N; i++) {
		CHECK(rp_rx(&p[i], &sg, f, 50 * MS) &&
			      sg.flags == (F_SYN | F_ACK),
		      "no SYN-ACK for %d", i);
		p[i].risn = sg.seq;
	}
	for (int i = 0; i < N; i++) {
		p[i].ts += 3;
		rp_tx(&p[i], F_ACK, p[i].iss + 1, p[i].risn + 1, 0, 512, 0);
		if (i % 8 == 7)
			while (NS[0]->accept(ls, NULL, NULL) >= 0)
				got++;
	}
	sim_run_for(10 * MS);
	while (NS[0]->accept(ls, NULL, NULL) >= 0)
		got++;
	CHECK(got == N, "accepted %d of %d", got, N);
	CHECK(NS[0]->counter(NSC_TCP_SYNCOOKIE_SENT) > 0, "no cookies used");
	return 0;
}

/* The accept queue holds backlog + 1 and the SYN queue as much; beyond
 * them SYNs get cookies and completions that find the accept queue full
 * are dropped (the clients' next segment tries again).  Once the server
 * accepts, every client is served -- its data included. */
static int t_acceptq_full(void)
{
	enum { N = 6 };
	int ls = NS[1]->socket(K_SOCK_STREAM), c[N], got = 0, bytes = 0;
	uint8_t b = 'q';

	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 2) == 0,
	      "listen");
	for (int i = 0; i < N; i++)
		c[i] = tcp_connect_nb(0, inst_ip(1), 8080);
	sim_run_for(500 * MS);
	CHECK(xmib(1, "ListenOverflows") > 0 ||
		      xmib(1, "TCPReqQFullDoCookies") > 0,
	      "no overflow counted");
	CHECK(NS[1]->counter(NSC_TCP_CONNS) <= 1 + 3 + 2,
	      "%llu entries for a queue of 3", NS[1]->counter(NSC_TCP_CONNS));
	for (int i = 0; i < N; i++)
		NS[0]->send(c[i], &b, 1, 0);
	for (int k = 0; k < 400 && got < N; k++) {
		sim_run_for(50 * MS);
		int a;
		while ((a = NS[1]->accept(ls, NULL, NULL)) >= 0) {
			uint8_t r[4];

			got++;
			NS[1]->set_nonblock(a, 1);
			sim_run_for(5 * SEC);
			if (NS[1]->recv(a, r, sizeof(r), 0) == 1)
				bytes++;
		}
	}
	CHECK(got == N, "accepted %d of %d", got, N);
	CHECK(bytes == N, "%d of %d requests arrived", bytes, N);
	return 0;
}

/* Closing a listener drops its request socks and resets the children
 * nobody accepted; their memory goes. */
static int t_listener_close(void)
{
	enum { N = 20 };
	struct rp p[N];
	struct tcpseg sg;
	uint8_t f[1700];
	int ls = NS[0]->socket(K_SOCK_STREAM);

	NS[0]->set_nonblock(ls, 1);
	CHECK(NS[0]->bind(ls, 0, 8080) == 0 && NS[0]->listen(ls, 64) == 0,
	      "listen");
	cap_start(0, 1);
	for (int i = 0; i < N; i++) {
		rp_init(&p[i], i, (uint16_t)(33000 + i), 8080);
		rp_tx(&p[i], F_SYN, p[i].iss, 0, 0, 65535, 1);
		CHECK(rp_rx(&p[i], &sg, f, 50 * MS), "no SYN-ACK %d", i);
		p[i].risn = sg.seq;
	}
	// five complete (children on the accept queue)
	for (int i = 0; i < 5; i++)
		rp_tx(&p[i], F_ACK, p[i].iss + 1, p[i].risn + 1, 0, 512, 0);
	sim_run_for(5 * MS);
	while (cap_next(f, sizeof(f), 0, NULL))
		;
	NS[0]->close(ls);
	sim_run_for(10 * MS);
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 0, "%llu entries left",
	      NS[0]->counter(NSC_TCP_CONNS));
	// the children got resets
	int rsts = 0;
	for (int i = 0; i < 5; i++)
		if (rp_rx(&p[i], &sg, f, 10 * MS) && (sg.flags & F_RST))
			rsts++;
	CHECK(rsts == 5, "%d of 5 children reset", rsts);
	CHECK(full_socks(0) == 0, "%u full sockets left", full_socks(0));
	// a late ACK for a dropped request sock: a reset
	rp_tx(&p[10], F_ACK, p[10].iss + 1, p[10].risn + 1, 0, 512, 0);
	CHECK(rp_rx(&p[10], &sg, f, 10 * MS) && (sg.flags & F_RST),
	      "no RST for an ACK after the listener closed");
	return 0;
}

/* TCP_DEFER_ACCEPT: the connection is accept()able when data arrives,
 * not at the bare ACK. */
static int t_defer_accept(void)
{
	int ls = NS[1]->socket(K_SOCK_STREAM), c, a;
	uint8_t b[16] = "hello";

	NS[1]->set_nonblock(ls, 1);
	CHECK(setint(1, ls, K_IPPROTO_TCP, K_TCP_DEFER_ACCEPT, 10) == 0,
	      "setsockopt");
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 8) == 0,
	      "listen");
	c = tcp_connect_nb(0, inst_ip(1), 8080);
	CHECK(wait_established(0, c, SEC) == 0, "client not established");
	sim_run_for(1500 * MS);
	a = NS[1]->accept(ls, NULL, NULL);
	CHECK(a < 0, "accepted before any data");
	CHECK(NS[0]->send(c, b, 5, 0) == 5, "send");
	sim_run_for(20 * MS);
	a = NS[1]->accept(ls, NULL, NULL);
	CHECK(a >= 0, "not accepted after data: %d", a);
	NS[1]->set_nonblock(a, 1);
	CHECK(NS[1]->recv(a, b, sizeof(b), 0) == 5, "data lost");
	return 0;
}

/* ======================================================================
 * TIME_WAIT and closing
 * ====================================================================== */

/* TIME_WAIT costs a small sock, not a socket. */
static int t_tw_minisock(void)
{
	enum { N = 100 };
	int ls = NS[1]->socket(K_SOCK_STREAM), c[N], s[N];

	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 256) == 0,
	      "listen");
	for (int i = 0; i < N; i++)
		c[i] = tcp_connect_nb(0, inst_ip(1), 8080);
	sim_run_for(100 * MS);
	for (int i = 0; i < N; i++) {
		s[i] = NS[1]->accept(ls, NULL, NULL);
		CHECK(s[i] >= 0, "accept %d", i);
	}
	for (int i = 0; i < N; i++)
		NS[0]->close(c[i]);
	sim_run_for(100 * MS);
	for (int i = 0; i < N; i++)
		NS[1]->close(s[i]);
	sim_run_for(500 * MS);
	CHECK(n0_kt_tcp_tw() == N, "%u TIME_WAIT socks", n0_kt_tcp_tw());
	CHECK(full_socks(0) == 0, "%u full sockets kept", full_socks(0));
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == N, "%llu table entries",
	      NS[0]->counter(NSC_TCP_CONNS));
	sim_run_for(61 * SEC);
	CHECK(n0_kt_tcp_tw() == 0 && NS[0]->counter(NSC_TCP_CONNS) == 0,
	      "TIME_WAIT left after 61 s: %u", n0_kt_tcp_tw());
	return 0;
}

/* connect() from the port of a TIME_WAIT sock to the same peer: reused
 * (the old connection had timestamps). */
static int t_tw_reuse_connect(void)
{
	int ls = NS[1]->socket(K_SOCK_STREAM), c, s;

	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 8) == 0,
	      "listen");
	for (int round = 0; round < 3; round++) {
		c = NS[0]->socket(K_SOCK_STREAM);
		NS[0]->set_nonblock(c, 1);
		setint(0, c, K_SOL_SOCKET, K_SO_REUSEADDR, 1);
		CHECK(NS[0]->bind(c, 0, 45000) == 0, "bind round %d", round);
		int r = NS[0]->connect(c, inst_ip(1), 8080);
		CHECK(r == 0 || r == -EINPROGRESS, "connect round %d: %d",
		      round, r);
		CHECK(wait_established(0, c, 2 * SEC) == 0 &&
			      NS[0]->tcp_state(c) == TS_ESTABLISHED,
		      "round %d not established (state %d)", round,
		      NS[0]->tcp_state(c));
		sim_run_for(5 * MS);
		s = NS[1]->accept(ls, NULL, NULL);
		CHECK(s >= 0, "accept round %d", round);
		NS[0]->close(c); // client first: TIME_WAIT on the client
		sim_run_for(50 * MS);
		NS[1]->close(s);
		sim_run_for(100 * MS);
		CHECK(n0_kt_tcp_tw() == 1, "round %d: %u TIME_WAIT", round,
		      n0_kt_tcp_tw());
		sim_run_for(2 * SEC);
	}
	CHECK(xmib(0, "TWReused") >= 2, "TIME_WAIT reused %llu times",
	      xmib(0, "TWReused"));
	return 0;
}

/* A new SYN for the 4-tuple of a TIME_WAIT sock with newer timestamps
 * reopens it on the server (the server closed first). */
static int t_tw_syn_recycle(void)
{
	int ls = NS[1]->socket(K_SOCK_STREAM), c, s;

	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 8) == 0,
	      "listen");
	for (int round = 0; round < 2; round++) {
		c = NS[0]->socket(K_SOCK_STREAM);
		NS[0]->set_nonblock(c, 1);
		CHECK(NS[0]->bind(c, 0, 46000) == 0, "bind");
		NS[0]->connect(c, inst_ip(1), 8080);
		CHECK(wait_established(0, c, 5 * SEC) == 0 &&
			      NS[0]->tcp_state(c) == TS_ESTABLISHED,
		      "round %d: state %d", round, NS[0]->tcp_state(c));
		sim_run_for(5 * MS);
		s = NS[1]->accept(ls, NULL, NULL);
		CHECK(s >= 0, "accept");
		NS[1]->close(s); // server first: TIME_WAIT on the server
		sim_run_for(50 * MS);
		NS[0]->close(c);
		sim_run_for(100 * MS);
		CHECK(n1_kt_tcp_tw() == 1, "server TIME_WAIT %u", n1_kt_tcp_tw());
		sim_run_for(SEC);
	}
	CHECK(xmib(1, "TWRecycled") >= 1, "TIME_WAIT never recycled");
	return 0;
}

/* An orphan in FIN_WAIT2 waits 60 s for the peer's FIN; a socket that
 * only shut down its sending side waits as long as it likes. */
static int t_finwait2_timeout(void)
{
	int c, s, c2, s2;

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	CHECK(tcp_pair(0, 1, 8081, &c2, &s2, NULL, 5 * SEC) == 0, "pair 2");
	NS[0]->close(c);
	NS[0]->shutdown(c2, K_SHUT_WR);
	sim_run_for(SEC);
	CHECK(NS[0]->tcp_state(c2) == TS_FIN_WAIT_2, "half-closed state %d",
	      NS[0]->tcp_state(c2));
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 2, "%llu entries",
	      NS[0]->counter(NSC_TCP_CONNS));
	sim_run_for(57 * SEC);
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 2,
	      "orphan FIN_WAIT2 gone before 58 s");
	sim_run_for(4 * SEC);
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 1,
	      "orphan FIN_WAIT2 still there after 62 s (%llu)",
	      NS[0]->counter(NSC_TCP_CONNS));
	sim_run_for(120 * SEC);
	CHECK(NS[0]->tcp_state(c2) == TS_FIN_WAIT_2,
	      "a half-closed socket's FIN_WAIT2 timed out (state %d)",
	      NS[0]->tcp_state(c2));
	// the peer's FIN still ends it normally
	NS[1]->close(s2);
	sim_run_for(SEC);
	CHECK(NS[0]->tcp_state(c2) == TS_CLOSED && n0_kt_tcp_tw() == 1,
	      "state %d, %u TIME_WAIT", NS[0]->tcp_state(c2), n0_kt_tcp_tw());
	return 0;
}

/* close() with unread data resets the connection (RFC 2525 2.17). */
static int t_rst_unread_close(void)
{
	int c, s;
	uint8_t buf[2000];

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	pat_fill(3, 0, buf, 1000);
	CHECK(NS[1]->send(s, buf, 1000, 0) == 1000, "send");
	sim_run_for(50 * MS);
	unsigned long long r0 = NS[0]->counter(NSC_TCP_OUT_RSTS);
	NS[0]->close(c);
	sim_run_for(50 * MS);
	CHECK(NS[0]->counter(NSC_TCP_OUT_RSTS) == r0 + 1, "no RST sent");
	int r = NS[1]->recv(s, buf, sizeof(buf), 0);
	CHECK(r == -ECONNRESET, "peer recv %d", r);
	CHECK(n0_kt_tcp_tw() == 0 && full_socks(0) == 0,
	      "the aborted connection lingers");
	return 0;
}

/* After close(), data from the peer has no reader: a reset. */
static int t_orphan_data_rst(void)
{
	int c, s;
	uint8_t buf[100] = { 0 };

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	NS[0]->close(c);
	sim_run_for(50 * MS);
	CHECK(NS[1]->send(s, buf, 50, 0) == 50, "send");
	sim_run_for(50 * MS);
	/* The reset is reported once -- here by the send; a reset in
	 * CLOSE_WAIT (our FIN came first) is EPIPE -- and the stream then
	 * reads end of file. */
	int w = NS[1]->send(s, buf, 50, 0);
	sim_run_for(50 * MS);
	int r = NS[1]->recv(s, buf, sizeof(buf), 0);
	CHECK(w == -EPIPE && r == 0, "peer not reset: send %d, recv %d", w, r);
	CHECK(so_error(1, s) == 0, "the error was reported twice");
	sim_run_for(SEC);
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 0, "%llu entries left",
	      NS[0]->counter(NSC_TCP_CONNS));
	return 0;
}

/* ======================================================================
 * ACKs
 * ====================================================================== */

/* Bulk data: one ACK for every two full-sized segments. */
static int t_ack_every_second(void)
{
	int c, s;
	struct path_cfg p = { 0 };

	p.delay_ms = 5;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	/* While the receive window grows every segment is ACKed (quick
	 * ACKs let the sender's window follow); measured after that. */
	CHECK(transfer(1, s, 0, c, 5, 20000000, 30 * SEC) == 20000000,
	      "warm-up transfer");
	tap_on();
	CHECK(transfer(1, s, 0, c, 6, 3000000, 30 * SEC) == 3000000,
	      "transfer");
	tap_off();
	unsigned data = 0, acks = 0;
	for (int i = 0; i < tapn; i++) {
		if (tapv[i].inst == 1 && tapv[i].len >= 1448)
			data++;
		if (tapv[i].inst == 0 && tapv[i].len == 0 &&
		    (tapv[i].flags & F_ACK))
			acks++;
	}
	double ratio = (double)acks / (double)data;
	tlog("%u data segments, %u ACKs (%.2f)", data, acks, ratio);
	CHECK(ratio > 0.40 && ratio < 0.62, "ACK ratio %.2f (%u/%u)", ratio,
	      acks, data);
	return 0;
}

/* Delayed ACKs: the first data is ACKed at once (quick ACK mode), later
 * single segments of a slow stream are delayed by the ACK timeout. */
static int t_delack_timing(void)
{
	int c, s;
	struct path_cfg p = { 0 };
	uint8_t b = 'x';

	p.delay_ms = 10;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	tap_on();
	for (int i = 0; i < 40; i++) {
		CHECK(NS[1]->send(s, &b, 1, 0) == 1, "send %d", i);
		sim_run_for(150 * MS);
		uint8_t r[8];
		NS[0]->recv(c, r, sizeof(r), 0);
	}
	tap_off();
	/* the delay of each ACK of instance 0 after the data it ACKs
	 * arrived (data left instance 1 10 ms before arriving) */
	int quick = 0, delayed = 0, bad = 0;
	for (int i = 0; i < tapn; i++) {
		if (tapv[i].inst != 1 || tapv[i].len != 1)
			continue;
		simtime_t arr = tapv[i].t + 10 * MS;
		for (int j = i + 1; j < tapn; j++) {
			if (tapv[j].inst == 0 && tapv[j].ack == tapv[i].seq + 1) {
				double d = (double)(tapv[j].t - arr) / 1e6;

				if (d < 6)
					quick++;
				else if (d >= 30 && d <= 210)
					delayed++;
				else
					bad++;
				break;
			}
		}
	}
	tlog("quick %d delayed %d other %d", quick, delayed, bad);
	CHECK(quick >= 1, "the first data was not ACKed at once");
	CHECK(delayed >= 15, "only %d delayed ACKs (quick %d, other %d)",
	      delayed, quick, bad);
	CHECK(bad == 0, "%d ACKs with an odd delay", bad);
	return 0;
}

/* TCP_QUICKACK turns delayed ACKs off for a while. */
static int t_quickack_option(void)
{
	int c, s;
	struct kt_tcp_sockinfo si;

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	CHECK(setint(0, c, K_IPPROTO_TCP, K_TCP_QUICKACK, 1) == 0,
	      "setsockopt");
	CHECK(sockinfo(0, c, &si) == 0 && si.ack_quick > 0 && !si.ack_pingpong,
	      "not in quick-ACK mode (quick %u pingpong %u)", si.ack_quick,
	      si.ack_pingpong);
	CHECK(setint(0, c, K_IPPROTO_TCP, K_TCP_QUICKACK, 0) == 0,
	      "setsockopt 0");
	CHECK(sockinfo(0, c, &si) == 0 && si.ack_pingpong, "pingpong not set");
	return 0;
}

/* PSH goes on the last segment of a write only. */
static int t_psh_last_segment(void)
{
	int c, s;
	static uint8_t buf[20000];
	struct path_cfg p = { 0 };

	p.delay_ms = 5;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	tap_on();
	pat_fill(9, 0, buf, sizeof(buf));
	CHECK(NS[0]->send(c, buf, sizeof(buf), 0) == (int)sizeof(buf), "send");
	sim_run_for(SEC);
	tap_off();
	int segs = 0, psh = 0, last_psh = 0;
	uint32_t end = 0;
	for (int i = 0; i < tapn; i++) {
		if (tapv[i].inst != 0 || !tapv[i].len)
			continue;
		segs++;
		if (tapv[i].flags & F_PSH) {
			psh++;
			if (tapv[i].seq + tapv[i].len > end)
				last_psh = 1;
		}
		if (tapv[i].seq + tapv[i].len > end)
			end = tapv[i].seq + tapv[i].len;
	}
	CHECK(segs >= 14, "%d segments", segs);
	CHECK(psh == 1 && last_psh, "%d of %d segments with PSH", psh, segs);
	uint8_t r[20000];
	int got = 0;
	for (int k = 0; k < 5; k++) {
		int n = NS[1]->recv(s, r + got, sizeof(r) - (size_t)got, 0);
		if (n > 0)
			got += n;
	}
	CHECK(got == (int)sizeof(buf) && !pat_check(9, 0, r, sizeof(r)),
	      "received %d", got);
	return 0;
}

/* ======================================================================
 * Windows
 * ====================================================================== */
static int last_window_ack(int inst, simtime_t after, simtime_t *when)
{
	for (int i = tapn - 1; i >= 0; i--) {
		if (tapv[i].inst == inst && tapv[i].t >= after &&
		    !tapv[i].len && (tapv[i].flags & F_ACK)) {
			if (when)
				*when = tapv[i].t;
			return (int)tapv[i].win;
		}
	}
	return -1;
}

/* A reader that stopped closes the window; reading a little does not
 * open it in small steps (receiver SWS avoidance), reading enough sends
 * a window update at once, and the transfer completes. */
static int t_window_update_on_read(void)
{
	int c, s;
	static uint8_t buf[300000];
	struct path_cfg p = { 0 };
	struct kt_tcp_sockinfo si;

	p.delay_ms = 5;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	pat_fill(4, 0, buf, sizeof(buf));
	tap_on();
	int sent = 0;
	for (int k = 0; k < 50; k++) {
		int r = NS[1]->send(s, buf + sent, sizeof(buf) - (size_t)sent, 0);
		if (r > 0)
			sent += r;
		sim_run_for(20 * MS);
	}
	CHECK(sockinfo(0, c, &si) == 0 && si.rcvq_bytes > 0, "nothing queued");
	sim_run_for(500 * MS);
	int w0 = last_window_ack(0, 0, NULL);
	CHECK(w0 == 0, "window not closed: %d (queued %u)", w0, si.rcvq_bytes);
	// a small read: no update
	uint8_t r[400000];
	simtime_t t0 = now_ns();
	CHECK(NS[0]->recv(c, r, 100, 0) == 100, "small read");
	sim_run_for(100 * MS);
	int w1 = last_window_ack(0, t0, NULL);
	CHECK(w1 <= 0, "a 100-byte read opened the window (%d)", w1);
	// read everything: an update right away
	t0 = now_ns();
	int got = 100;
	int n = NS[0]->recv(c, r + got, sizeof(r) - (size_t)got, 0);
	CHECK(n > 0, "read");
	got += n;
	simtime_t tw = 0;
	int w2 = -1;
	for (int i = 0; i < tapn; i++)
		if (tapv[i].inst == 0 && tapv[i].t >= t0 && tapv[i].win > 0) {
			w2 = (int)tapv[i].win;
			tw = tapv[i].t;
			break;
		}
	if (w2 < 0) {
		sim_run_for(10 * MS);
		for (int i = 0; i < tapn; i++)
			if (tapv[i].inst == 0 && tapv[i].t >= t0 &&
			    tapv[i].win > 0) {
				w2 = (int)tapv[i].win;
				tw = tapv[i].t;
				break;
			}
	}
	CHECK(w2 > 0 && tw - t0 <= 6 * MS, "no prompt window update (%d after %.1f ms)",
	      w2, (double)(tw - t0) / 1e6);
	tap_off();
	for (int k = 0; k < 400 && got < (int)sizeof(buf); k++) {
		if (sent < (int)sizeof(buf)) {
			int w = NS[1]->send(s, buf + sent,
					    sizeof(buf) - (size_t)sent, 0);
			if (w > 0)
				sent += w;
		}
		sim_run_for(10 * MS);
		n = NS[0]->recv(c, r + got, sizeof(r) - (size_t)got, 0);
		if (n > 0)
			got += n;
	}
	CHECK(got == (int)sizeof(buf) && !pat_check(4, 0, r, sizeof(buf)),
	      "got %d of %zu", got, sizeof(buf));
	return 0;
}

/* Nagle (TCP_NODELAY off): small writes while data is unacknowledged go
 * out together. */
static int t_nagle_coalesce(void)
{
	int c, s;
	uint8_t b[10] = "0123456789";
	struct path_cfg p = { 0 };

	p.delay_ms = 50;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	CHECK(setint(0, c, K_IPPROTO_TCP, K_TCP_NODELAY, 0) == 0, "nodelay 0");
	tap_on();
	for (int i = 0; i < 50; i++)
		CHECK(NS[0]->send(c, b, 10, 0) == 10, "send %d", i);
	sim_run_for(SEC);
	tap_off();
	int segs = 0;
	for (int i = 0; i < tapn; i++)
		if (tapv[i].inst == 0 && tapv[i].len)
			segs++;
	CHECK(segs <= 3, "%d segments for 50 small writes", segs);
	uint8_t r[600];
	CHECK(NS[1]->recv(s, r, sizeof(r), 0) == 500, "data lost");
	return 0;
}

/* TCP_CORK holds a partial segment back until uncorked. */
static int t_cork_hold(void)
{
	int c, s, on = 1, off = 0;
	uint8_t b[100] = { 0 };

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	NS[0]->setsockopt(c, K_IPPROTO_TCP, K_TCP_CORK, &on, sizeof(on));
	tap_on();
	CHECK(NS[0]->send(c, b, 100, 0) == 100, "send");
	sim_run_for(100 * MS);
	int segs = 0;
	for (int i = 0; i < tapn; i++)
		if (tapv[i].inst == 0 && tapv[i].len)
			segs++;
	CHECK(segs == 0, "corked data sent");
	NS[0]->setsockopt(c, K_IPPROTO_TCP, K_TCP_CORK, &off, sizeof(off));
	sim_run_for(5 * MS);
	segs = 0;
	for (int i = 0; i < tapn; i++)
		if (tapv[i].inst == 0 && tapv[i].len)
			segs++;
	tap_off();
	CHECK(segs == 1, "%d segments after uncorking", segs);
	return 0;
}

/* ======================================================================
 * Buffers
 * ====================================================================== */

/* A fast reader at a high bandwidth-delay product: the receive buffer
 * grows to megabytes, the send buffer follows the congestion window, and
 * the throughput is far beyond a 128 KB window's. */
static int t_autotune_bdp(void)
{
	int c, s;
	struct path_cfg p = { 0 };
	struct kt_tcp_sockinfo rs, ss;

	p.delay_ms = 75;
	link_set_both(&p);
	link_cfg(0)->rate_mbps = 1000;
	link_cfg(1)->rate_mbps = 1000;
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	simtime_t t0 = now_ns();
	long long got = transfer(1, s, 0, c, 6, 40000000, 60 * SEC);
	double secs = (double)(now_ns() - t0) / 1e9;
	double mbps = (double)got * 8 / secs / 1e6;
	CHECK(sockinfo(0, c, &rs) == 0 && sockinfo(1, s, &ss) == 0,
	      "sockinfo");
	tlog("%.1f Mbit/s, rcvbuf %d window_clamp %u, sndbuf %d cwnd %u",
	     mbps, rs.rcvbuf, rs.window_clamp, ss.sndbuf, ss.cwnd);
	CHECK(got == 40000000, "transfer %lld", got);
	CHECK(rs.rcvbuf > 2 * 1024 * 1024, "rcvbuf only %d", rs.rcvbuf);
	CHECK(ss.sndbuf > 1024 * 1024, "sndbuf only %d", ss.sndbuf);
	CHECK(mbps > 50, "only %.1f Mbit/s", mbps);
	return 0;
}

/* SO_RCVBUF fixes the buffer: no auto-tuning, and the window scale of the
 * SYN is chosen for it. */
static int t_rcvbuf_locked(void)
{
	int c, s;
	struct kt_tcp_sockinfo si;
	struct path_cfg p = { 0 };
	int v = 16384;

	p.delay_ms = 20;
	link_set_both(&p);
	c = NS[0]->socket(K_SOCK_STREAM);
	NS[0]->set_nonblock(c, 1);
	CHECK(NS[0]->setsockopt(c, K_SOL_SOCKET, K_SO_RCVBUF, &v, sizeof(v)) ==
		      0,
	      "SO_RCVBUF");
	int ls = NS[1]->socket(K_SOCK_STREAM);
	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 8) == 0,
	      "listen");
	tap_on();
	NS[0]->connect(c, inst_ip(1), 8080);
	CHECK(wait_established(0, c, 2 * SEC) == 0, "connect");
	sim_run_for(50 * MS);
	s = NS[1]->accept(ls, NULL, NULL);
	CHECK(s >= 0, "accept");
	NS[1]->set_nonblock(s, 1);
	tap_off();
	int syn_ws = -1;
	for (int i = 0; i < tapn; i++)
		if (tapv[i].inst == 0 && tapv[i].flags == F_SYN)
			syn_ws = tapv[i].ws;
	CHECK(syn_ws >= 0 && syn_ws <= 1, "SYN window scale %d for 32 KB",
	      syn_ws);
	CHECK(transfer(1, s, 0, c, 7, 2000000, 30 * SEC) == 2000000,
	      "transfer");
	CHECK(sockinfo(0, c, &si) == 0 && si.rcvbuf == 32768 &&
		      (si.userlocks & 2),
	      "rcvbuf %d locks %u", si.rcvbuf, si.userlocks);
	return 0;
}

/* The defaults of a SYN: window 64240, scale 7, MSS 1460. */
static int t_syn_defaults(void)
{
	uint8_t f[1600];
	struct tcpseg sg;
	unsigned char m1[6];

	inst_mac(1, m1);
	NS[0]->arp_add(inst_ip(1), m1);
	cap_start(0, 1);
	int c = tcp_connect_nb(0, inst_ip(1), 8080);
	CHECK(c >= 0, "socket");
	unsigned n = cap_next_proto(6, f, sizeof(f), 100 * MS);
	CHECK(n && !pkt_parse_tcp(f, n, &sg) && sg.flags == F_SYN, "no SYN");
	CHECK(sg.win == 64240 && sg.ws == 7 && sg.mss == 1460 && sg.sackok &&
		      sg.ts,
	      "SYN win %u ws %d mss %d sack %d ts %d", sg.win, sg.ws, sg.mss,
	      sg.sackok, sg.ts);
	return 0;
}

/* TCP_MAXSEG before connect(): the SYN offers it, and the segments sent
 * respect it. */
static int t_maxseg(void)
{
	int c = NS[0]->socket(K_SOCK_STREAM), s, v;
	unsigned len = sizeof(v);
	/* Within the initial send buffer (two initial windows of
	 * segments): the SYN-ACK no longer grows the window. */
	static uint8_t buf[15000];

	NS[0]->set_nonblock(c, 1);
	CHECK(setint(0, c, K_IPPROTO_TCP, K_TCP_MAXSEG, 1000) == 0,
	      "TCP_MAXSEG");
	int ls = NS[1]->socket(K_SOCK_STREAM);
	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 8) == 0,
	      "listen");
	tap_on();
	NS[0]->connect(c, inst_ip(1), 8080);
	CHECK(wait_established(0, c, 2 * SEC) == 0, "connect");
	sim_run_for(10 * MS);
	s = NS[1]->accept(ls, NULL, NULL);
	CHECK(s >= 0, "accept");
	NS[1]->set_nonblock(s, 1);
	CHECK(NS[0]->send(c, buf, sizeof(buf), 0) == (int)sizeof(buf), "send");
	sim_run_for(SEC);
	tap_off();
	int syn_mss = -1;
	unsigned maxlen = 0;
	for (int i = 0; i < tapn; i++) {
		if (tapv[i].inst != 0)
			continue;
		if (tapv[i].flags == F_SYN)
			syn_mss = tapv[i].mss;
		if (tapv[i].len > maxlen)
			maxlen = tapv[i].len;
	}
	CHECK(syn_mss == 1000, "SYN MSS %d", syn_mss);
	CHECK(maxlen <= 1000 && maxlen > 900, "largest segment %u", maxlen);
	CHECK(NS[0]->getsockopt(c, K_IPPROTO_TCP, K_TCP_MAXSEG, &v, &len) == 0 &&
		      v <= 1000,
	      "TCP_MAXSEG reads %d", v);
	return 0;
}

/* TCP_NOTSENT_LOWAT: not writable while that much waits unsent. */
static int t_notsent_lowat(void)
{
	int c, s;
	static uint8_t buf[200000];

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	CHECK(setint(0, c, K_IPPROTO_TCP, K_TCP_NOTSENT_LOWAT, 4096) == 0,
	      "setsockopt");
	int sent = 0;
	for (int k = 0; k < 20; k++) {
		int r = NS[0]->send(c, buf, sizeof(buf), 0);
		if (r > 0)
			sent += r;
		sim_run_for(20 * MS);
	}
	// the reader never reads: the window closed, data waits unsent
	CHECK(!(NS[0]->poll(c, K_POLLOUT) & K_POLLOUT),
	      "writable with lots unsent (sent %d)", sent);
	uint8_t r[400000];
	for (int k = 0; k < 50; k++) {
		NS[1]->recv(s, r, sizeof(r), 0);
		sim_run_for(20 * MS);
	}
	CHECK(NS[0]->poll(c, K_POLLOUT) & K_POLLOUT,
	      "not writable once everything went out");
	return 0;
}

/* TCP_USER_TIMEOUT: a connection whose peer went silent fails after it,
 * not after the full retransmission budget. */
static int t_user_timeout(void)
{
	int c, s;
	uint8_t b[100] = { 0 };

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	CHECK(setint(0, c, K_IPPROTO_TCP, K_TCP_USER_TIMEOUT, 3000) == 0,
	      "setsockopt");
	tap_on();
	tap_drop_inst = 0; // a black hole
	simtime_t t0 = now_ns();
	CHECK(NS[0]->send(c, b, 100, 0) == 100, "send");
	int r = 0;
	for (int k = 0; k < 200; k++) {
		sim_run_for(100 * MS);
		if (NS[0]->tcp_state(c) == TS_CLOSED)
			break;
	}
	tap_off();
	double secs = (double)(now_ns() - t0) / 1e9;
	r = so_error(0, c);
	CHECK(NS[0]->tcp_state(c) == TS_CLOSED, "still %d after %.1f s",
	      NS[0]->tcp_state(c), secs);
	CHECK(secs > 2.5 && secs < 6, "failed after %.1f s", secs);
	CHECK(r == ETIMEDOUT, "SO_ERROR %d", r);
	return 0;
}

/* SO_KEEPALIVE with short times: probes, then the connection fails. */
static int t_keepalive_probes(void)
{
	int c, s;

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	CHECK(setint(0, c, K_IPPROTO_TCP, K_TCP_KEEPIDLE, 2) == 0 &&
		      setint(0, c, K_IPPROTO_TCP, K_TCP_KEEPINTVL, 1) == 0 &&
		      setint(0, c, K_IPPROTO_TCP, K_TCP_KEEPCNT, 3) == 0 &&
		      setint(0, c, K_SOL_SOCKET, K_SO_KEEPALIVE, 1) == 0,
	      "setsockopt");
	// alive: the probes are answered
	sim_run_for(10 * SEC);
	CHECK(NS[0]->tcp_state(c) == TS_ESTABLISHED,
	      "keepalive killed a live connection");
	CHECK(xmib(0, "TCPKeepAlive") >= 3, "%llu probes sent",
	      xmib(0, "TCPKeepAlive"));
	tap_on();
	tap_drop_inst = 1; // the peer goes silent
	simtime_t t0 = now_ns();
	for (int k = 0; k < 100 && NS[0]->tcp_state(c) != TS_CLOSED; k++)
		sim_run_for(100 * MS);
	tap_off();
	double secs = (double)(now_ns() - t0) / 1e9;
	CHECK(NS[0]->tcp_state(c) == TS_CLOSED, "not closed after %.1f s",
	      secs);
	CHECK(secs < 7, "closed after %.1f s", secs);
	CHECK(so_error(0, c) == ETIMEDOUT, "SO_ERROR %d", so_error(0, c));
	return 0;
}

/* ======================================================================
 * The receive queues (raw peer)
 * ====================================================================== */
static int rp_listen_connect(struct rp *p, int *ls, int *a)
{
	*ls = NS[0]->socket(K_SOCK_STREAM);
	NS[0]->set_nonblock(*ls, 1);
	if (NS[0]->bind(*ls, 0, 8080) || NS[0]->listen(*ls, 16))
		return -1;
	cap_start(0, 1);
	rp_init(p, 3, 34000, 8080);
	if (rp_connect(p) < 0)
		return -1;
	sim_run_for(5 * MS);
	*a = NS[0]->accept(*ls, NULL, NULL);
	if (*a < 0)
		return -1;
	NS[0]->set_nonblock(*a, 1);
	return 0;
}

/* Out of order with overlaps and duplicates: D-SACK for what was had
 * twice, everything delivered once, intact. */
static int t_ooo_overlap_dsack(void)
{
	struct rp p;
	struct tcpseg sg;
	uint8_t f[1700], r[8000];
	int ls, a;
	uint32_t base;

	CHECK(rp_listen_connect(&p, &ls, &a) == 0, "setup");
	base = p.iss + 1;
	p.ts += 5;
	rp_tx(&p, F_ACK, base + 2000, p.risn + 1, 1000, 512, 0); // A
	CHECK(rp_rx(&p, &sg, f, 20 * MS) && sg.nsack == 1 &&
		      sg.sack[0][0] == base + 2000 &&
		      sg.sack[0][1] == base + 3000,
	      "SACK for A: %d blocks", sg.nsack);
	rp_tx(&p, F_ACK, base + 2500, p.risn + 1, 1000, 512, 0); // B overlaps
	CHECK(rp_rx(&p, &sg, f, 20 * MS) && sg.nsack >= 1, "ACK for B");
	CHECK(sg.sack[0][0] == base + 2500 && sg.sack[0][1] == base + 3000,
	      "no D-SACK for B's overlap: %u..%u", sg.sack[0][0] - base,
	      sg.sack[0][1] - base);
	rp_tx(&p, F_ACK, base + 2000, p.risn + 1, 1000, 512, 0); // A again
	CHECK(rp_rx(&p, &sg, f, 20 * MS) && sg.nsack >= 2 &&
		      sg.sack[0][0] == base + 2000 &&
		      sg.sack[0][1] == base + 3000 &&
		      sg.sack[1][0] == base + 2000 &&
		      sg.sack[1][1] == base + 3500,
	      "duplicate: D-SACK then the block (%d blocks)", sg.nsack);
	rp_tx(&p, F_ACK | F_PSH, base, p.risn + 1, 1000, 512, 0);
	rp_tx(&p, F_ACK | F_PSH, base + 1000, p.risn + 1, 1000, 512, 0);
	sim_run_for(10 * MS);
	int n = NS[0]->recv(a, r, sizeof(r), 0);
	CHECK(n == 3500 && !pat_check(77, 0, r, 3500), "recv %d", n);
	return 0;
}

/* PAWS: a segment with an older timestamp is dropped (and ACKed). */
static int t_paws(void)
{
	struct rp p;
	struct tcpseg sg;
	uint8_t f[1700], r[2000];
	int ls, a;
	uint32_t base;

	CHECK(rp_listen_connect(&p, &ls, &a) == 0, "setup");
	base = p.iss + 1;
	p.ts = 5000;
	rp_tx(&p, F_ACK | F_PSH, base, p.risn + 1, 100, 512, 0);
	sim_run_for(10 * MS);
	CHECK(NS[0]->recv(a, r, sizeof(r), 0) == 100, "first data");
	while (cap_next(f, sizeof(f), 0, NULL))
		;
	p.ts = 4000; // older
	rp_tx(&p, F_ACK | F_PSH, base + 100, p.risn + 1, 100, 512, 0);
	CHECK(rp_rx(&p, &sg, f, 20 * MS) && sg.ack == base + 100,
	      "no duplicate ACK for the old segment");
	CHECK(NS[0]->recv(a, r, sizeof(r), 0) == -EAGAIN,
	      "an old segment's data was delivered");
	p.ts = 6000;
	rp_tx(&p, F_ACK | F_PSH, base + 100, p.risn + 1, 100, 512, 0);
	sim_run_for(10 * MS);
	CHECK(NS[0]->recv(a, r, sizeof(r), 0) == 100, "fresh data lost");
	return 0;
}

/* RFC 5961: in-window resets not at rcv_nxt draw (rate limited)
 * challenge ACKs and do not reset; the exact one does. */
static int t_challenge_ack(void)
{
	struct rp p;
	struct tcpseg sg;
	uint8_t f[1700];
	int ls, a, acks = 0;
	uint32_t base;

	CHECK(rp_listen_connect(&p, &ls, &a) == 0, "setup");
	base = p.iss + 1;
	while (cap_next(f, sizeof(f), 0, NULL))
		;
	for (int i = 0; i < 50; i++)
		rp_tx(&p, F_RST, base + 100 + (uint32_t)i, 0, 0, 0, 0);
	while (rp_rx(&p, &sg, f, 5 * MS))
		acks++;
	CHECK(acks >= 1 && acks <= 2, "%d challenge ACKs for 50 resets", acks);
	CHECK(NS[0]->tcp_state(a) == TS_ESTABLISHED, "reset by a blind RST");
	rp_tx(&p, F_RST, base, 0, 0, 0, 0);
	sim_run_for(5 * MS);
	CHECK(NS[0]->tcp_state(a) == TS_CLOSED, "the exact RST was ignored");
	return 0;
}

/* Out-of-order data that cannot be merged and overruns the receive
 * buffer: pruned, the connection lives on and delivers everything once
 * the sender resends. */
static int t_ooo_prune(void)
{
	struct rp p;
	uint8_t f[1700];
	static uint8_t r[200000];
	int ls, a;
	uint32_t base;
	struct kt_tcp_sockinfo si;

	CHECK(rp_listen_connect(&p, &ls, &a) == 0, "setup");
	base = p.iss + 1;
	// 150 islands of 100 bytes, each 400 apart: never contiguous
	for (int i = 1; i <= 150; i++) {
		p.ts++;
		rp_tx(&p, F_ACK, base + (uint32_t)i * 400, p.risn + 1, 100,
		      512, 0);
	}
	while (cap_next(f, sizeof(f), 0, NULL))
		;
	CHECK(sockinfo(0, a, &si) == 0, "sockinfo");
	tlog("ooo skbs %u rmem %u rcvbuf %d", si.ooo_skbs, si.rmem_alloc,
	     si.rcvbuf);
	CHECK(si.rmem_alloc <= (unsigned)si.rcvbuf + 4096,
	      "rmem %u over rcvbuf %d", si.rmem_alloc, si.rcvbuf);
	CHECK(n0_kt_tcp_xmib("PruneCalled") > 0 ||
		      n0_kt_tcp_xmib("TCPOFODrop") > 0,
	      "nothing pruned");
	// the sender resends everything in order
	uint32_t got = 0;
	for (uint32_t off = 0; off < 61000; off += 1000) {
		p.ts++;
		rp_tx(&p, F_ACK, base + off, p.risn + 1, 1000, 512, 0);
		sim_run_for(MS);
		int n = NS[0]->recv(a, r + got, sizeof(r) - got, 0);
		if (n > 0)
			got += (uint32_t)n;
	}
	sim_run_for(10 * MS);
	int n = NS[0]->recv(a, r + got, sizeof(r) - got, 0);
	if (n > 0)
		got += (uint32_t)n;
	CHECK(got == 61000 && !pat_check(77, 0, r, got),
	      "delivered %u of 61000 (intact %d)", got,
	      got ? !pat_check(77, 0, r, got) : 0);
	return 0;
}

/* ======================================================================
 * Scale
 * ====================================================================== */

/* A thousand connections at once: all work, and an idle established
 * connection costs a few KB, not hundreds. */
static int t_conns_1000(void)
{
	enum { N = 1000 };
	static int c[N], s[N];
	static struct flow fl[2 * N];
	int ls = NS[1]->socket(K_SOCK_STREAM), got = 0;

	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 4096) == 0,
	      "listen");
	unsigned long long h0 = NS[1]->counter(NSC_KALLOC_BYTES);
	for (int i = 0; i < N; i++) {
		c[i] = tcp_connect_nb(0, inst_ip(1), 8080);
		CHECK(c[i] >= 0, "connect %d: %d", i, c[i]);
		if (i % 50 == 49) {
			sim_run_for(5 * MS);
			int a;
			while ((a = NS[1]->accept(ls, NULL, NULL)) >= 0) {
				NS[1]->set_nonblock(a, 1);
				s[got++] = a;
			}
		}
	}
	for (int k = 0; k < 100 && got < N; k++) {
		sim_run_for(20 * MS);
		int a;
		while ((a = NS[1]->accept(ls, NULL, NULL)) >= 0) {
			NS[1]->set_nonblock(a, 1);
			s[got++] = a;
		}
	}
	CHECK(got == N, "accepted %d of %d", got, N);
	sim_run_for(100 * MS);
	unsigned long long h1 = NS[1]->counter(NSC_KALLOC_BYTES);
	double per = (double)(h1 - h0) / N;
	printf("    memory per idle established connection: %.0f bytes\n",
	       per);
	CHECK(per < 8192, "%.0f bytes per connection", per);
	for (int i = 0; i < N; i++) {
		unsigned ip, cport, sport;
		int match = -1;

		NS[0]->sockname(c[i], 0, &ip, &cport);
		for (int j = 0; j < N && match < 0; j++) {
			NS[1]->sockname(s[j], 1, &ip, &sport);
			if (sport == cport)
				match = j;
		}
		CHECK(match >= 0, "no server end for %d", i);
		flow_init(&fl[2 * i], 0, c[i], 1, s[match], 1000 + (unsigned)i,
			  3000);
		flow_init(&fl[2 * i + 1], 1, s[match], 0, c[i],
			  3000 + (unsigned)i, 5000);
	}
	int ok = flows_run(fl, 2 * N, 60 * SEC);
	CHECK(ok == 2 * N, "%d of %d flows complete", ok, 2 * N);
	return 0;
}

#define T(n, f, ...) { .name = n, .group = "unit", .fn = f, __VA_ARGS__ }
const struct test tests_tcp[] = {
	T("tcp-rbtree", t_rbtree, .ninst = 1),
	T("tcp-reqsk-small", t_reqsk_small, .ninst = 1),
	T("tcp-synack-backoff", t_synack_backoff, .ninst = 1),
	T("tcp-syncookie", t_syncookie, .ninst = 1),
	T("tcp-synflood-valid-acks", t_synflood_valid_acks, .ninst = 1),
	T("tcp-acceptq-full", t_acceptq_full),
	T("tcp-listener-close", t_listener_close, .ninst = 1),
	T("tcp-defer-accept", t_defer_accept),
	T("tcp-tw-minisock", t_tw_minisock),
	T("tcp-tw-reuse-connect", t_tw_reuse_connect),
	T("tcp-tw-syn-recycle", t_tw_syn_recycle),
	T("tcp-finwait2-timeout", t_finwait2_timeout),
	T("tcp-rst-unread-close", t_rst_unread_close),
	T("tcp-orphan-data-rst", t_orphan_data_rst),
	T("tcp-ack-every-second", t_ack_every_second),
	T("tcp-delack-timing", t_delack_timing),
	T("tcp-quickack-option", t_quickack_option),
	T("tcp-psh-last-segment", t_psh_last_segment),
	T("tcp-window-update-on-read", t_window_update_on_read),
	T("tcp-nagle-coalesce", t_nagle_coalesce),
	T("tcp-cork-hold", t_cork_hold),
	T("tcp-autotune-bdp", t_autotune_bdp),
	T("tcp-rcvbuf-locked", t_rcvbuf_locked),
	T("tcp-syn-defaults", t_syn_defaults),
	T("tcp-maxseg", t_maxseg),
	T("tcp-notsent-lowat", t_notsent_lowat),
	T("tcp-user-timeout", t_user_timeout),
	T("tcp-keepalive-probes", t_keepalive_probes),
	T("tcp-ooo-overlap-dsack", t_ooo_overlap_dsack, .ninst = 1),
	T("tcp-paws", t_paws, .ninst = 1),
	T("tcp-challenge-ack", t_challenge_ack, .ninst = 1),
	T("tcp-ooo-prune", t_ooo_prune, .ninst = 1),
	T("tcp-conns-1000", t_conns_1000),
	{ 0 },
};
