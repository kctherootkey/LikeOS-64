/* Two-stack interoperation tests: instance 0 and instance 1 talk over the
 * simulated wire through their real socket layers.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "tests.h"
#include "pkt.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- shared checks ---- */

/* After everything is closed: every connection is gone (TIME_WAIT
 * included) and the kernel heap is back where it started. */
struct heap_base {
	unsigned long long live[NINST];
};

static void heap_mark(struct heap_base *b)
{
	for (int i = 0; i < NINST; i++)
		b->live[i] = NS[i]->counter(NSC_KALLOC_LIVE);
}

static int settle_clean(const struct heap_base *b, simtime_t wait)
{
	sim_run_for(wait);
	for (int i = 0; i < NINST; i++) {
		unsigned long long c = NS[i]->counter(NSC_TCP_CONNS);
		unsigned long long l = NS[i]->counter(NSC_KALLOC_LIVE);
		CHECK(c == 0, "instance %d still has %llu connections", i, c);
		CHECK(l == b->live[i],
		      "instance %d heap: %llu blocks live, %llu before", i, l,
		      b->live[i]);
	}
	return 0;
}

static int verify_tap_bad;
static unsigned long long verify_tap_frames;

static int verify_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	(void)inst;
	(void)ctx;
	verify_tap_frames++;
	if (frame_csum_bad(f, len)) {
		if (!verify_tap_bad)
			printf("    instance %d sent a frame with a bad checksum "
			       "(proto %d, len %u)\n",
			       inst, pkt_proto(f, len), len);
		verify_tap_bad++;
	}
	return 0;
}

static void conn_diag(const char *what, int inst, int s)
{
	struct ns_tcpinfo ti;

	memset(&ti, 0, sizeof(ti));
	NS[inst]->tcpinfo(s, &ti);
	printf("    %s: state %d rto %u us rtt %u us cwnd %u ssthresh %u "
	       "unacked %u retrans %u\n",
	       what, NS[inst]->tcp_state(s), ti.rto_us, ti.rtt_us, ti.snd_cwnd,
	       ti.snd_ssthresh, ti.unacked, ti.total_retrans);
}

static void link_diag(void)
{
	for (int d = 0; d < 2; d++) {
		const struct link_stats *l = link_stats(d);
		printf("    link %d->%d: %llu frames, %llu delivered, lost %llu, "
		       "queue drops %llu, dup %llu, reordered %llu\n",
		       d, d ^ 1, l->tx_frames, l->delivered, l->drop_loss,
		       l->drop_queue, l->dup, l->reordered);
	}
}

static int pair(int *c, int *s)
{
	return tcp_pair(0, 1, 8080, c, s, NULL, 10 * SEC);
}

/* ---- connection setup ---- */

static int t_handshake(void)
{
	struct heap_base hb;
	int c, s;
	unsigned ip, port;

	heap_mark(&hb);
	CHECK(pair(&c, &s) == 0, "no connection");
	CHECK(NS[0]->tcp_state(c) == TS_ESTABLISHED, "client state %d",
	      NS[0]->tcp_state(c));
	CHECK(NS[1]->tcp_state(s) == TS_ESTABLISHED, "server state %d",
	      NS[1]->tcp_state(s));
	/* what a nonblocking client does first (getpeername before it is
	 * the separate xfail test getpeername-nonblock) */
	CHECK(so_error(0, c) == 0, "SO_ERROR %d", so_error(0, c));
	CHECK(NS[0]->sockname(c, 1, &ip, &port) == 0 && ip == inst_ip(1) &&
		      port == 8080,
	      "client peer %x:%u", ip, port);
	CHECK(NS[1]->sockname(s, 0, &ip, &port) == 0 && ip == inst_ip(1) &&
		      port == 8080,
	      "server name %x:%u", ip, port);
	unsigned cport = 0;
	NS[0]->sockname(c, 0, &ip, &cport);
	CHECK(ip == inst_ip(0) && cport >= 49152, "client name %x:%u", ip,
	      cport);
	CHECK(NS[1]->sockname(s, 1, &ip, &port) == 0 && ip == inst_ip(0) &&
		      port == cport,
	      "server peer %x:%u", ip, port);
	CHECK(so_error(0, c) == 0, "SO_ERROR %d", so_error(0, c));
	NS[0]->close(c);
	NS[1]->close(s);
	return settle_clean(&hb, 130 * SEC);
}

static int t_refused(void)
{
	int c = tcp_connect_nb(0, inst_ip(1), 9999);

	CHECK(c >= 0, "socket %d", c);
	sim_run_for(2 * SEC);
	CHECK(so_error(0, c) == ECONNREFUSED, "SO_ERROR %d", so_error(0, c));
	NS[0]->close(c);
	return 0;
}

/* a SYN that is lost is retransmitted and the connection completes */
static int t_syn_lost(void)
{
	int c, s;

	link_cfg(0)->path[0].burst_period_ms = 1e9; /* one window: */
	link_cfg(0)->path[0].burst_len_ms = 1500; /* first 1.5 s lost */
	link_cfg(0)->path[0].burst_offset_ms = 0;
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 30 * SEC) == 0,
	      "no connection after a lost SYN");
	CHECK(link_stats(0)->drop_loss >= 1, "nothing was lost");
	return 0;
}

/* ---- data ---- */

static int bulk(unsigned long long n, simtime_t limit, int rev)
{
	int c, s;

	CHECK(pair(&c, &s) == 0, "no connection");
	wire_set_tap(verify_tap, NULL);
	long long got = rev ? transfer(1, s, 0, c, 1, n, limit) :
			      transfer(0, c, 1, s, 1, n, limit);
	wire_set_tap(NULL, NULL);
	if (got != (long long)n) {
		conn_diag(rev ? "receiver" : "sender", 0, c);
		conn_diag(rev ? "sender" : "receiver", 1, s);
		link_diag();
		printf("    at %.3f s, missed wakeups %llu\n", now_s(),
		       g_missed_wakeups);
	}
	CHECK(got == (long long)n, "moved %lld of %llu bytes", got, n);
	CHECK(!verify_tap_bad, "%d frames with bad checksums", verify_tap_bad);
	tlog("bulk: %llu bytes in %.3f s", n, now_s());
	return 0;
}

static int t_bulk(void)
{
	return bulk(4000000, 120 * SEC, 0);
}

static int t_bulk_reverse(void)
{
	return bulk(4000000, 120 * SEC, 1);
}

static int t_bulk_bidir(void)
{
	int c, s;
	struct flow f[2];

	CHECK(pair(&c, &s) == 0, "no connection");
	flow_init(&f[0], 0, c, 1, s, 11, 3000000);
	flow_init(&f[1], 1, s, 0, c, 12, 3000000);
	int ok = flows_run(f, 2, 120 * SEC);
	CHECK(ok == 2, "%d of 2 directions complete (%llu, %llu bytes)", ok,
	      f[0].got, f[1].got);
	return 0;
}

static int t_bulk_rtt(void)
{
	struct path_cfg p = { 0 };

	p.delay_ms = 25;
	link_set_both(&p);
	link_cfg(0)->rate_mbps = link_cfg(1)->rate_mbps = 100;
	link_cfg(0)->queue_bytes = link_cfg(1)->queue_bytes = 256 * 1024;
	return bulk(4000000, 300 * SEC, 0);
}

static int impaired(const struct path_cfg *p, unsigned long long n)
{
	link_set_both(p);
	link_cfg(0)->rate_mbps = link_cfg(1)->rate_mbps = 100;
	link_cfg(0)->queue_bytes = link_cfg(1)->queue_bytes = 256 * 1024;
	return bulk(n, 600 * SEC, 0);
}

static int t_loss(void)
{
	struct path_cfg p = { 0 };

	p.delay_ms = 10;
	p.loss = 0.01;
	return impaired(&p, 2000000);
}

static int t_loss_burst(void)
{
	struct path_cfg p = { 0 };

	p.delay_ms = 10;
	p.ge_p_gb = 0.005;
	p.ge_p_bg = 0.3;
	p.ge_loss_bad = 0.7;
	return impaired(&p, 2000000);
}

static int t_reorder(void)
{
	struct path_cfg p = { 0 };

	p.delay_ms = 10;
	p.reorder = 0.05;
	p.reorder_ms = 3;
	return impaired(&p, 2000000);
}

static int t_duplicate(void)
{
	struct path_cfg p = { 0 };

	p.delay_ms = 10;
	p.dup = 0.05;
	return impaired(&p, 2000000);
}

static int t_corrupt(void)
{
	struct path_cfg p = { 0 };

	p.delay_ms = 5;
	p.corrupt = 0.02;
	return impaired(&p, 2000000);
}

static int t_small_mtu(void)
{
	return bulk(1000000, 120 * SEC, 0);
}

/* the receiver stops reading: the sender fills the window, gets EAGAIN,
 * probes the zero window, and everything arrives once reading resumes */
static int t_zero_window(void)
{
	int c, s;
	uint8_t *buf = malloc(65536);
	unsigned long long sent = 0;

	CHECK(pair(&c, &s) == 0, "no connection");
	for (int round = 0; round < 400; round++) {
		pat_fill(5, sent, buf, 65536);
		int r = NS[0]->send(c, buf, 65536, 0);
		if (r > 0)
			sent += (unsigned)r;
		sim_run_for(50 * MS);
	}
	CHECK(sent > 0 && sent < 20000000, "sent %llu with nobody reading",
	      sent);
	int r = NS[0]->send(c, buf, 1000, 0);
	CHECK(r == -EAGAIN, "send into a full window returned %d", r);
	unsigned long long probes0 = NS[0]->counter(NSC_TCP_OUT_SEGS);
	sim_run_for(20 * SEC);
	unsigned long long probes = NS[0]->counter(NSC_TCP_OUT_SEGS) - probes0;
	CHECK(probes >= 1, "no window probe in 20 s");
	CHECK(NS[0]->tcp_state(c) == TS_ESTABLISHED, "state %d",
	      NS[0]->tcp_state(c));
	free(buf);
	/* now read it all */
	struct flow f;
	flow_init(&f, 0, c, 1, s, 5, sent);
	f.sent = sent; /* already sent; only receive */
	int ok = flows_run(&f, 1, 120 * SEC);
	CHECK(ok == 1, "received %llu of %llu after the window opened",
	      f.got, sent);
	return 0;
}

/* ---- closing ---- */

static int close_variant(int who)
{
	struct heap_base hb;
	int c, s;
	char b[64];

	heap_mark(&hb);
	CHECK(pair(&c, &s) == 0, "no connection");
	CHECK(transfer(0, c, 1, s, 2, 10000, 10 * SEC) == 10000, "data");
	if (who == 0) { /* client first */
		NS[0]->close(c);
		sim_run_for(SEC);
		CHECK(NS[1]->recv(s, b, sizeof(b), 0) == 0, "server sees no EOF");
		NS[1]->close(s);
	} else if (who == 1) { /* server first */
		NS[1]->close(s);
		sim_run_for(SEC);
		CHECK(NS[0]->recv(c, b, sizeof(b), 0) == 0, "client sees no EOF");
		NS[0]->close(c);
	} else { /* both at the same instant */
		NS[0]->close(c);
		NS[1]->close(s);
	}
	return settle_clean(&hb, 130 * SEC);
}

static int t_close_client(void)
{
	return close_variant(0);
}

static int t_close_server(void)
{
	return close_variant(1);
}

static int t_close_simultaneous(void)
{
	return close_variant(2);
}

/* client: request, shutdown(SHUT_WR), read the response promptly */
static int t_half_close(void)
{
	int c, s;
	struct flow req, resp;

	CHECK(pair(&c, &s) == 0, "no connection");
	flow_init(&req, 0, c, 1, s, 21, 5000);
	req.close_after = 1;
	CHECK(flows_run(&req, 1, 10 * SEC) == 1 && req.eof,
	      "request %llu bytes, eof %d", req.got, req.eof);
	sim_run_for(500 * MS);
	CHECK(NS[0]->tcp_state(c) == TS_FIN_WAIT_2, "client state %d",
	      NS[0]->tcp_state(c));
	flow_init(&resp, 1, s, 0, c, 22, 200000);
	resp.close_after = 2;
	CHECK(flows_run(&resp, 1, 30 * SEC) == 1,
	      "response: %llu of 200000 bytes (err %d)", resp.got, resp.err);
	char b[16];
	sim_run_for(SEC);
	CHECK(NS[0]->recv(c, b, sizeof(b), 0) == 0, "no EOF");
	NS[0]->close(c);
	return 0;
}

static int t_abort_rst(void)
{
	int c, s;
	int lg[2] = { 1, 0 };
	char b[16];

	CHECK(pair(&c, &s) == 0, "no connection");
	CHECK(NS[0]->setsockopt(c, K_SOL_SOCKET, K_SO_LINGER, lg, sizeof(lg)) ==
		      0,
	      "SO_LINGER");
	NS[0]->close(c);
	sim_run_for(SEC);
	int r = NS[1]->recv(s, b, sizeof(b), 0);
	CHECK(r == -ECONNRESET, "peer recv after RST: %d", r);
	CHECK(NS[1]->send(s, b, 1, 0) < 0, "send after RST succeeded");
	NS[1]->close(s);
	return 0;
}

/* ---- many connections ---- */

static int t_many_concurrent(void)
{
	enum { N = 200 };
	int ls, cs[N], ss[N];
	struct flow *f = calloc(2 * N, sizeof(*f));

	ls = NS[1]->socket(K_SOCK_STREAM);
	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 128) == 0,
	      "listen");
	int accepted = 0;
	for (int i = 0; i < N; i++) {
		cs[i] = tcp_connect_nb(0, inst_ip(1), 8080);
		CHECK(cs[i] >= 0, "connect %d: %d", i, cs[i]);
		/* accept as they arrive, like a server loop */
		sim_run_for(MS);
		int a;
		while ((a = NS[1]->accept(ls, NULL, NULL)) >= 0) {
			NS[1]->set_nonblock(a, 1);
			ss[accepted++] = a;
		}
	}
	/* the strict variant wants every one of the burst within 2 s (the
	 * request socks take them all; nothing waits for a SYN resend) */
	int waits = g_cur_test->arg ? 40 : 1200;
	for (int k = 0; k < waits && accepted < N; k++) {
		sim_run_for(50 * MS);
		int a;
		while ((a = NS[1]->accept(ls, NULL, NULL)) >= 0) {
			NS[1]->set_nonblock(a, 1);
			ss[accepted++] = a;
		}
	}
	CHECK(accepted == N, "accepted %d of %d", accepted, N);
	/* pair them by the client's port */
	for (int i = 0; i < N; i++) {
		unsigned ip, cport, sport;
		NS[0]->sockname(cs[i], 0, &ip, &cport);
		int match = -1;
		for (int j = 0; j < N; j++) {
			NS[1]->sockname(ss[j], 1, &ip, &sport);
			if (sport == cport)
				match = j;
		}
		CHECK(match >= 0, "no server end for client %d", i);
		flow_init(&f[2 * i], 0, cs[i], 1, ss[match], 100 + i, 20000);
		flow_init(&f[2 * i + 1], 1, ss[match], 0, cs[i], 500 + i, 50000);
	}
	int ok = flows_run(f, 2 * N, 120 * SEC);
	CHECK(ok == 2 * N, "%d of %d flows complete", ok, 2 * N);
	for (int i = 0; i < N; i++) {
		NS[0]->close(cs[i]);
		NS[1]->close(ss[i]);
	}
	free(f);
	return 0;
}

static int t_many_sequential(void)
{
	struct heap_base hb;
	int ls = NS[1]->socket(K_SOCK_STREAM);

	heap_mark(&hb);
	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 64) == 0,
	      "listen");
	for (int i = 0; i < 300; i++) {
		int c = tcp_connect_nb(0, inst_ip(1), 8080), s = -1;
		CHECK(c >= 0, "connect %d: %d", i, c);
		for (int k = 0; k < 100 && s < 0; k++) {
			sim_run_for(MS);
			s = NS[1]->accept(ls, NULL, NULL);
		}
		CHECK(s >= 0, "connection %d not accepted", i);
		NS[1]->set_nonblock(s, 1);
		CHECK(wait_established(0, c, 5 * SEC) == 0, "connection %d", i);
		CHECK(transfer(1, s, 0, c, (unsigned)i, 3000, 5 * SEC) == 3000,
		      "connection %d data", i);
		NS[0]->close(c);
		NS[1]->close(s);
		sim_run_for(20 * MS);
	}
	NS[1]->close(ls);
	return settle_clean(&hb, 130 * SEC);
}

/* ---- UDP and ICMP ---- */

static int udp_roundtrip(unsigned len)
{
	int a = NS[0]->socket(K_SOCK_DGRAM), b = NS[1]->socket(K_SOCK_DGRAM);
	uint8_t *buf = malloc(65536), *rb = malloc(65536);
	unsigned ip, port;

	CHECK(a >= 0 && b >= 0, "sockets");
	NS[0]->set_nonblock(a, 1);
	NS[1]->set_nonblock(b, 1);
	CHECK(NS[1]->bind(b, 0, 5353) == 0, "bind");
	CHECK(NS[0]->bind(a, 0, 6000) == 0, "bind");
	wire_set_tap(verify_tap, NULL);
	for (unsigned i = 0; i < 10; i++) {
		pat_fill(30 + i, 0, buf, len);
		int r = NS[0]->sendto(a, buf, len, 0, inst_ip(1), 5353);
		CHECK(r == (int)len, "sendto %u: %d", len, r);
		sim_run_for(10 * MS);
		r = NS[1]->recvfrom(b, rb, 65536, 0, &ip, &port);
		CHECK(r == (int)len, "datagram %u: received %d of %u", i, r, len);
		CHECK(ip == inst_ip(0) && port == 6000, "from %x:%u", ip, port);
		CHECK(!pat_check(30 + i, 0, rb, len), "datagram %u corrupt", i);
		/* and back */
		r = NS[1]->sendto(b, rb, (unsigned)r, 0, ip, port);
		CHECK(r == (int)len, "reply %d", r);
		sim_run_for(10 * MS);
		r = NS[0]->recvfrom(a, rb, 65536, 0, &ip, &port);
		CHECK(r == (int)len && !pat_check(30 + i, 0, rb, len),
		      "reply %u: %d", i, r);
	}
	wire_set_tap(NULL, NULL);
	CHECK(!verify_tap_bad, "%d frames with bad checksums", verify_tap_bad);
	NS[0]->close(a);
	NS[1]->close(b);
	free(buf);
	free(rb);
	return 0;
}

static int t_udp(void)
{
	return udp_roundtrip(512);
}

static int t_udp_fragmented(void)
{
	return udp_roundtrip(3000);
}

static int t_udp_large(void)
{
	return udp_roundtrip(8000);
}

static int t_udp_burst(void)
{
	int a = NS[0]->socket(K_SOCK_DGRAM), b = NS[1]->socket(K_SOCK_DGRAM);
	uint8_t buf[256];
	int got = 0;

	NS[1]->set_nonblock(b, 1);
	CHECK(NS[1]->bind(b, 0, 5353) == 0, "bind");
	for (int i = 0; i < 12; i++) {
		pat_fill((unsigned)i, 0, buf, 100);
		CHECK(NS[0]->sendto(a, buf, 100, 0, inst_ip(1), 5353) == 100,
		      "sendto");
	}
	sim_run_for(50 * MS);
	while (NS[1]->recvfrom(b, buf, sizeof(buf), 0, NULL, NULL) == 100)
		got++;
	CHECK(got == 12, "received %d of 12 queued datagrams", got);
	return 0;
}

static int t_udp_unreachable(void)
{
	int a = NS[0]->socket(K_SOCK_DGRAM);
	uint8_t buf[64] = { 0 };

	NS[0]->set_nonblock(a, 1);
	CHECK(NS[0]->connect(a, inst_ip(1), 7777) == 0, "connect");
	CHECK(NS[0]->send(a, buf, 10, 0) == 10, "send");
	sim_run_for(100 * MS);
	CHECK(NS[1]->counter(NSC_UDP_NO_PORTS) == 1, "no-port count %llu",
	      NS[1]->counter(NSC_UDP_NO_PORTS));
	int r = NS[0]->send(a, buf, 10, 0);
	CHECK(r == -ECONNREFUSED, "send after port unreachable: %d", r);
	return 0;
}

static int t_icmp_echo(void)
{
	uint8_t data[56];

	pat_fill(9, 0, data, sizeof(data));
	wire_set_tap(verify_tap, NULL);
	for (unsigned seq = 1; seq <= 3; seq++) {
		CHECK(NS[0]->ping(inst_ip(1), 0x4242, seq, data, sizeof(data)) >=
			      0,
		      "ping send");
		CHECK(NS[0]->ping_reply(0x4242, seq) == 1, "no reply to %u", seq);
	}
	wire_set_tap(NULL, NULL);
	CHECK(!verify_tap_bad, "%d frames with bad checksums", verify_tap_bad);
	return 0;
}

/* Allocations fail now and then (kernel heap and packet buffers) while
 * connections come and go and data moves: nothing may crash, and once the
 * failures stop the stacks work normally. */
static int t_alloc_faults(void)
{
	int ls = NS[1]->socket(K_SOCK_STREAM);

	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->bind(ls, 0, 8080) == 0 && NS[1]->listen(ls, 64) == 0,
	      "listen");
	for (int i = 0; i < 2; i++) {
		NS[i]->alloc_fail(3, 7);
		NS[i]->skb_fail(5, 13);
		NS[i]->quiet(1);
	}
	for (int round = 0; round < 40; round++) {
		int c = tcp_connect_nb(0, inst_ip(1), 8080), s = -1;
		sim_run_for(50 * MS);
		s = NS[1]->accept(ls, NULL, NULL);
		if (c >= 0 && s >= 0) {
			NS[1]->set_nonblock(s, 1);
			transfer(0, c, 1, s, (unsigned)round, 30000, 2 * SEC);
			transfer(1, s, 0, c, (unsigned)round + 100, 30000, 2 * SEC);
		}
		if (c >= 0)
			NS[0]->close(c);
		if (s >= 0)
			NS[1]->close(s);
		int u = NS[0]->socket(K_SOCK_DGRAM);
		uint8_t d[2000] = { 0 };
		if (u >= 0) {
			NS[0]->sendto(u, d, sizeof(d), 0, inst_ip(1), 9);
			NS[0]->close(u);
		}
	}
	for (int i = 0; i < 2; i++) {
		NS[i]->alloc_fail(0, 0);
		NS[i]->skb_fail(0, 0);
		NS[i]->quiet(0);
	}
	sim_run_for(130 * SEC);
	int c, s;
	CHECK(tcp_pair(0, 1, 8081, &c, &s, NULL, 10 * SEC) == 0,
	      "no connection after the failures stopped");
	CHECK(transfer(0, c, 1, s, 3, 500000, 30 * SEC) == 500000,
	      "data after the failures stopped");
	return 0;
}

#define T(n, f, ...) { .name = n, .group = "interop", .fn = f, __VA_ARGS__ }
const struct test tests_interop[] = {
	T("handshake", t_handshake),
	T("refused", t_refused),
	T("syn-lost", t_syn_lost),
	T("bulk", t_bulk),
	T("bulk-reverse", t_bulk_reverse),
	T("bulk-bidir", t_bulk_bidir),
	T("bulk-rtt50", t_bulk_rtt),
	T("loss-1pct", t_loss),
	T("loss-burst", t_loss_burst),
	T("reorder", t_reorder),
	T("duplicate", t_duplicate),
	T("corrupt", t_corrupt),
	T("mtu-576", t_small_mtu, .mtu = 576),
	T("zero-window", t_zero_window),
	T("close-client-first", t_close_client),
	T("close-server-first", t_close_server),
	T("close-simultaneous", t_close_simultaneous),
	T("half-close", t_half_close),
	T("abort-rst", t_abort_rst),
	T("many-concurrent", t_many_concurrent),
	T("many-concurrent-burst", t_many_concurrent, .arg = "strict"),
	T("many-sequential", t_many_sequential),
	T("udp", t_udp),
	T("udp-fragmented", t_udp_fragmented),
	T("udp-large", t_udp_large),
	T("udp-burst", t_udp_burst),
	T("udp-unreachable", t_udp_unreachable),
	T("icmp-echo", t_icmp_echo),
	T("alloc-faults", t_alloc_faults, .allow_warn = 1),
	{ 0 },
};
