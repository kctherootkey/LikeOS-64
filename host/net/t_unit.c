/* Per-module tests of the network stack: crafted frames into one
 * instance's receive path with what it transmits captured, and the known
 * bugs (expected failures) the overhaul's wave 2 fixes.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "tests.h"
#include "pkt.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t zero6[6] = { 0 };
static const uint8_t hostx_mac[6] = { 0x02, 0, 0, 0, 0, 0x09 };
#define HOSTX_IP ((10u << 24) | 9)
static const uint8_t router_mac[6] = { 0x02, 0, 0, 0, 0, 0xfe };
#define ROUTER_IP ((10u << 24) | 254)

static void my_mac(int i, uint8_t m[6])
{
	unsigned char t[6];

	inst_mac(i, t);
	memcpy(m, t, 6);
}

/* ---- ARP ---- */

static int t_arp_reply(void)
{
	uint8_t f[1600], m0[6];
	unsigned n;

	my_mac(0, m0);
	cap_start(0, 1);
	n = pkt_arp(f, 1, hostx_mac, HOSTX_IP, zero6, inst_ip(0), bcast);
	NS[0]->rx(f, n);
	n = cap_next_proto(0x806, f, sizeof(f), 100 * MS);
	CHECK(n >= 42, "no ARP reply");
	CHECK(!memcmp(f, hostx_mac, 6), "reply not to the asker");
	CHECK(f[20] == 0 && f[21] == 2, "opcode %d", f[21]);
	CHECK(!memcmp(f + 22, m0, 6), "sender MAC is not ours");
	CHECK(!memcmp(f + 32, hostx_mac, 6), "target MAC");
	/* a request for someone else: no reply */
	n = pkt_arp(f, 1, hostx_mac, HOSTX_IP, zero6, inst_ip(0) + 50, bcast);
	NS[0]->rx(f, n);
	CHECK(!cap_next_proto(0x806, f, sizeof(f), 100 * MS),
	      "answered an ARP request for another address");
	return 0;
}

static int t_arp_resolve(void)
{
	uint8_t f[1600], m0[6], buf[100];
	unsigned n;
	int s = NS[0]->socket(K_SOCK_DGRAM);

	my_mac(0, m0);
	cap_start(0, 1);
	memset(buf, 0x11, sizeof(buf));
	CHECK(NS[0]->sendto(s, buf, 100, 0, HOSTX_IP, 7000) >= 0, "sendto");
	n = cap_next_proto(0x806, f, sizeof(f), 100 * MS);
	CHECK(n >= 42, "no ARP request for an unknown neighbour");
	CHECK(!memcmp(f, bcast, 6) && f[21] == 1, "not a broadcast request");
	CHECK(f[38] == 10 && f[41] == 9, "asks for the wrong address");
	n = pkt_arp(f, 2, hostx_mac, HOSTX_IP, m0, inst_ip(0), m0);
	NS[0]->rx(f, n);
	n = cap_next_proto(17, f, sizeof(f), 100 * MS);
	CHECK(n, "the queued datagram was not sent after the ARP reply");
	CHECK(!memcmp(f, hostx_mac, 6), "datagram to the wrong MAC");
	CHECK(!frame_csum_bad(f, n), "bad checksum");
	return 0;
}

/* ---- IPv4 ---- */

static int echo_request(uint8_t *f, uint16_t seq, unsigned len)
{
	uint8_t m0[6], data[1500];

	my_mac(0, m0);
	pat_fill(seq, 0, data, len);
	return (int)pkt_icmp_echo(f, m0, hostx_mac, HOSTX_IP, inst_ip(0), 8,
				  0x77, seq, data, len);
}

static int t_icmp_echo_reply(void)
{
	uint8_t f[1600];
	unsigned n;

	NS[0]->arp_add(HOSTX_IP, hostx_mac);
	cap_start(0, 1);
	n = (unsigned)echo_request(f, 1, 100);
	NS[0]->rx(f, n);
	n = cap_next_proto(1, f, sizeof(f), 100 * MS);
	CHECK(n, "no echo reply");
	CHECK(!frame_csum_bad(f, n), "bad checksum in the reply");
	CHECK(f[34] == 0, "type %d", f[34]);
	CHECK(f[38] == 0 && f[39] == 0x77 && f[41] == 1, "id/seq not echoed");
	CHECK(n == 14 + 20 + 8 + 100 && !pat_check(1, 0, f + 42, 100),
	      "payload not echoed (len %u)", n);
	return 0;
}

static int t_ip_bad_header_csum(void)
{
	uint8_t f[1600];
	unsigned n;

	NS[0]->arp_add(HOSTX_IP, hostx_mac);
	cap_start(0, 1);
	n = (unsigned)echo_request(f, 2, 64);
	f[14 + 10] ^= 0x01; /* the IP checksum */
	unsigned long long e0 = NS[0]->counter(NSC_IP_IN_HDR_ERRORS);
	NS[0]->rx(f, n);
	CHECK(!cap_next_proto(1, f, sizeof(f), 100 * MS),
	      "answered a packet with a bad header checksum");
	CHECK(NS[0]->counter(NSC_IP_IN_HDR_ERRORS) > e0, "not counted");
	return 0;
}

static int t_ip_truncated(void)
{
	uint8_t f[1600];
	unsigned n;

	NS[0]->arp_add(HOSTX_IP, hostx_mac);
	cap_start(0, 1);
	n = (unsigned)echo_request(f, 3, 200);
	/* the frame ends 100 bytes before total_length says */
	NS[0]->rx(f, n - 100);
	CHECK(!cap_next_proto(1, f, sizeof(f), 100 * MS),
	      "answered a truncated packet");
	return 0;
}

/* total_length smaller than the header itself: must be dropped without
 * reading out of bounds (an ASan report is the failure) */
static int t_ip_totlen_below_ihl(void)
{
	uint8_t f[1600], m0[6], tcp[20] = { 0 };
	unsigned n;

	my_mac(0, m0);
	for (int proto = 0; proto < 3; proto++) {
		static const uint8_t protos[3] = { 6, 17, 1 };
		n = pkt_ip(f, m0, hostx_mac, HOSTX_IP, inst_ip(0),
			   protos[proto], tcp, sizeof(tcp), 9, 0, 64);
		f[14 + 2] = 0;
		f[14 + 3] = 10; /* total_length 10 < ihl 20 */
		f[14 + 10] = f[14 + 11] = 0;
		uint16_t c = inet_csum(f + 14, 20, 0);
		f[14 + 10] = (uint8_t)(c >> 8);
		f[14 + 11] = (uint8_t)c;
		NS[0]->rx(f, 14 + 10);
		NS[0]->rx(f, n);
	}
	sim_run_for(100 * MS);
	return 0;
}

static int t_ip_reassembly(void)
{
	uint8_t f[1600], m0[6], icmp[3008], data[3000];
	unsigned n;

	my_mac(0, m0);
	NS[0]->arp_add(HOSTX_IP, hostx_mac);
	cap_start(0, 1);
	/* a 3000-byte echo request in three fragments, last one first */
	pat_fill(4, 0, data, sizeof(data));
	icmp[0] = 8;
	icmp[1] = 0;
	icmp[2] = icmp[3] = 0;
	icmp[4] = 0;
	icmp[5] = 0x77;
	icmp[6] = 0;
	icmp[7] = 4;
	memcpy(icmp + 8, data, sizeof(data));
	uint16_t c = inet_csum(icmp, sizeof(icmp), 0);
	icmp[2] = (uint8_t)(c >> 8);
	icmp[3] = (uint8_t)c;
	unsigned offs[3] = { 2400, 1200, 0 };
	for (int i = 0; i < 3; i++) {
		unsigned o = offs[i], len = o == 2400 ? 3008 - 2400 : 1200;
		uint16_t frag = (uint16_t)(o / 8) | (o == 2400 ? 0 : 0x2000);
		n = pkt_ip(f, m0, hostx_mac, HOSTX_IP, inst_ip(0), 1, icmp + o,
			   len, 0x1234, frag, 64);
		NS[0]->rx(f, n);
	}
	/* the reply is fragmented too: collect the first fragment */
	n = cap_next_proto(1, f, sizeof(f), 200 * MS);
	CHECK(n, "no reply to a reassembled echo request");
	CHECK(f[34] == 0, "not an echo reply");
	return 0;
}

/* ---- TCP ---- */

/* Instance fix_inst picks `target' as the ISN of the next connection: the
 * 12-byte SipHash is fixed to 0, a probe connection at this instant shows
 * the time component, and the hash is then set so that the sum is the
 * target -- at the same instant (zero link delay, neighbours known).
 * Returns the established pair. */
static int pair_isn(int fix_inst, uint32_t target, int *c, int *s)
{
	uint8_t f[1600];
	struct tcpseg sg;
	unsigned char m[6];
	unsigned n;

	for (int i = 0; i < 2; i++) {
		inst_mac(i ^ 1, m);
		NS[i]->arp_add(inst_ip(i ^ 1), m);
	}
	NS[fix_inst]->isn_fix(1, 0);
	int ls = NS[1]->socket(K_SOCK_STREAM);
	NS[1]->set_nonblock(ls, 1);
	NS[1]->bind(ls, 0, 9);
	NS[1]->listen(ls, 4);
	cap_start(fix_inst, 0);
	tcp_connect_nb(0, inst_ip(1), 9);
	sim_run_until_time(now_ns());
	n = cap_next_proto(6, f, sizeof(f), 0);
	if (!n || pkt_parse_tcp(f, n, &sg) || !(sg.flags & F_SYN)) {
		printf("    no probe SYN\n");
		return -1;
	}
	NS[fix_inst]->isn_fix(1, target - sg.seq);
	cap_start(fix_inst, 0);
	simtime_t t0 = now_ns();
	if (tcp_pair(0, 1, 8080, c, s, NULL, 5 * SEC))
		return -1;
	NS[fix_inst]->isn_fix(0, 0);
	while ((n = cap_next_proto(6, f, sizeof(f), 0)) &&
	       !(pkt_parse_tcp(f, n, &sg) == 0 && (sg.flags & F_SYN) &&
		 sg.dport != 9 && sg.sport != 9))
		;
	cap_stop();
	if (!n || sg.seq != target) {
		printf("    setup: ISN %08x, wanted %08x (t %llu -> %llu)\n",
		       n ? sg.seq : 0, target, t0, now_ns());
		return -1;
	}
	return 0;
}

static int seq_wrap(int fix_inst)
{
	int c, s;
	struct flow f[2];

	CHECK(pair_isn(fix_inst, 0xFFFFFFFFu - 600000u, &c, &s) == 0,
	      "setup");
	flow_init(&f[0], 0, c, 1, s, 41, 2000000);
	flow_init(&f[1], 1, s, 0, c, 42, 2000000);
	int ok = flows_run(f, 2, 120 * SEC);
	CHECK(ok == 2, "across the wrap: %llu and %llu of 2000000 bytes",
	      f[0].got, f[1].got);
	return 0;
}

static int t_seq_wrap_0(void)
{
	return seq_wrap(0);
}

static int t_seq_wrap_1(void)
{
	return seq_wrap(1);
}

/* both ends connect to each other at the same time */
static int t_simultaneous_open(void)
{
	int a = NS[0]->socket(K_SOCK_STREAM), b = NS[1]->socket(K_SOCK_STREAM);
	struct path_cfg p = { 0 };

	p.delay_ms = 20;
	link_set_both(&p);
	NS[0]->set_nonblock(a, 1);
	NS[1]->set_nonblock(b, 1);
	CHECK(NS[0]->bind(a, 0, 5000) == 0 && NS[1]->bind(b, 0, 6000) == 0,
	      "bind");
	NS[0]->connect(a, inst_ip(1), 6000);
	NS[1]->connect(b, inst_ip(0), 5000);
	sim_run_for(5 * SEC);
	CHECK(NS[0]->tcp_state(a) == TS_ESTABLISHED &&
		      NS[1]->tcp_state(b) == TS_ESTABLISHED,
	      "states %d and %d", NS[0]->tcp_state(a), NS[1]->tcp_state(b));
	CHECK(so_error(0, a) == 0 && so_error(1, b) == 0, "SO_ERROR %d %d",
	      so_error(0, a), so_error(1, b));
	CHECK(transfer(0, a, 1, b, 3, 100000, 10 * SEC) == 100000, "data");
	return 0;
}

static int oob_frames, oob_bad;

static int oob_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	struct tcpseg s;

	(void)ctx;
	if (inst == 0 && !pkt_parse_tcp(f, len, &s) && (s.flags & F_URG)) {
		oob_frames++;
		if (!s.csum_ok)
			oob_bad++;
	}
	return 0;
}

static int t_oob_checksum(void)
{
	int c, s;
	uint8_t b = 'X';

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	wire_set_tap(oob_tap, NULL);
	int r = NS[0]->send_oob(c, b) >= 0 ? 1 : 0;
	sim_run_for(SEC);
	wire_set_tap(NULL, NULL);
	CHECK(r == 1, "send(MSG_OOB) %d", r);
	CHECK(oob_frames >= 1, "no URG segment");
	CHECK(oob_bad == 0, "%d of %d URG segments have a bad checksum",
	      oob_bad, oob_frames);
	return 0;
}

/* send(MSG_OOB) of three bytes: the urgent pointer points just past the
 * last of them, and the retransmission of the segment (the first copy is
 * dropped) carries URG, the pointer and the timestamp option again */
static int urg_frames, urg_good, urg_dropped;

static int urg_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	struct tcpseg s;

	(void)ctx;
	if (inst != 0 || pkt_parse_tcp(f, len, &s) || s.len != 3)
		return 0;
	urg_frames++;
	if ((s.flags & F_URG) && s.urg == 3 && s.ts && s.csum_ok)
		urg_good++;
	if (!urg_dropped) {
		urg_dropped = 1;
		return 1;
	}
	return 0;
}

static int t_send_oob(void)
{
	int c, s;
	uint8_t b[3] = { 'a', 'b', 'c' }, rb[8];

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	wire_set_tap(urg_tap, NULL);
	int r = NS[0]->send(c, b, 3, K_MSG_OOB);
	sim_run_for(3 * SEC);
	wire_set_tap(NULL, NULL);
	CHECK(r == 3, "send(MSG_OOB) %d", r);
	CHECK(urg_frames == 2, "%d copies of the segment, expected 2",
	      urg_frames);
	CHECK(urg_good == 2, "%d of %d copies with URG, pointer 3 and TS",
	      urg_good, urg_frames);
	NS[1]->set_nonblock(s, 1);
	r = NS[1]->recv(s, rb, sizeof(rb), 0);
	CHECK(r >= 2, "receiver got %d bytes", r);
	return 0;
}

/* TIME_WAIT lasts 60 s (2 MSL, what the constant was written for) */
static int t_time_wait_60s(void)
{
	int c, s;

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	NS[0]->close(c);
	sim_run_for(500 * MS);
	NS[1]->close(s);
	sim_run_for(500 * MS);
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 1, "no TIME_WAIT connection");
	sim_run_for(58 * SEC);
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 1,
	      "TIME_WAIT over after %.1f s", 59.0);
	sim_run_for(3 * SEC);
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 0,
	      "TIME_WAIT still there after 62 s");
	return 0;
}

/* SYN retransmissions: 1 s initial RTO, doubling (RFC 6298), whatever
 * the tick rate */
static int t_syn_rto(void)
{
	uint8_t f[1600];
	simtime_t t[4];
	unsigned char m1[6];

	inst_mac(1, m1);
	NS[0]->arp_add(inst_ip(1), m1);
	cap_start(0, 1); /* the SYNs go nowhere */
	int c = tcp_connect_nb(0, inst_ip(1), 8080);
	CHECK(c >= 0, "socket");
	for (int i = 0; i < 4; i++) {
		unsigned n;
		do
			n = cap_next(f, sizeof(f), 20 * SEC, &t[i]);
		while (n && pkt_proto(f, n) != 6);
		CHECK(n, "SYN %d not sent", i);
	}
	for (int i = 1; i < 4; i++) {
		double dt = (double)(t[i] - t[i - 1]) / 1e9;
		double want = (double)(1 << (i - 1));
		CHECK(dt > want - 0.02 && dt < want + 0.02,
		      "SYN retransmission %d after %.3f s, expected %.0f s", i,
		      dt, want);
	}
	return 0;
}

/* an ICMP host unreachable for an established connection is a soft
 * error: the connection carries on */
static int t_icmp_soft_error(void)
{
	int c, s;
	uint8_t f[1600], q[28], m0[6];
	struct tcpseg sg;

	my_mac(0, m0);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	cap_start(0, 0);
	uint8_t d[100];
	pat_fill(1, 0, d, sizeof(d));
	CHECK(NS[0]->send(c, d, sizeof(d), 0) == 100, "send");
	unsigned n = cap_next_proto(6, f, sizeof(f), 100 * MS);
	cap_stop();
	CHECK(n && !pkt_parse_tcp(f, n, &sg), "no data segment");
	memcpy(q, f + 14, 28); /* IP header + 8 bytes of TCP */
	for (int code = 0; code <= 1; code++) {
		n = pkt_icmp_err(f, m0, router_mac, ROUTER_IP, inst_ip(0), 3,
				 (uint8_t)code, 0, q, sizeof(q));
		NS[0]->rx(f, n);
	}
	n = pkt_icmp_err(f, m0, router_mac, ROUTER_IP, inst_ip(0), 11, 0, 0, q,
			 sizeof(q));
	NS[0]->rx(f, n);
	sim_run_for(100 * MS);
	CHECK(NS[0]->tcp_state(c) == TS_ESTABLISHED, "state %d after the "
						     "ICMP errors",
	      NS[0]->tcp_state(c));
	int pr = NS[0]->poll(c, K_POLLIN | K_POLLOUT);
	CHECK(!(pr & (K_POLLERR | K_POLLHUP)), "poll reports 0x%x", pr);
	uint8_t rb[100];
	int rr = NS[0]->recv(c, rb, sizeof(rb), 0);
	CHECK(rr == -EAGAIN, "recv after the soft errors: %d", rr);
	rr = NS[0]->send(c, rb, 1, 0);
	CHECK(rr == 1, "send after the soft errors: %d", rr);
	CHECK(NS[1]->recv(s, rb, sizeof(rb), 0) == 100 &&
		      !pat_check(1, 0, rb, 100),
	      "the first 100 bytes");
	sim_run_for(100 * MS);
	CHECK(NS[1]->recv(s, rb, 1, 0) == 1, "the byte after the errors");
	CHECK(transfer(0, c, 1, s, 2, 50000, 10 * SEC) == 50000,
	      "connection dead after a soft error");
	return 0;
}

static int t_second_connect(void)
{
	int c = tcp_connect_nb(0, inst_ip(1), 8080);
	struct path_cfg p = { 0 };

	p.delay_ms = 50;
	link_set_both(&p);
	CHECK(c >= 0, "connect");
	int r = NS[0]->connect(c, inst_ip(1), 8080);
	CHECK(r == -EALREADY, "second connect while in progress: %d", r);
	return 0;
}

static int t_getpeername_nonblock(void)
{
	int c, s;
	unsigned ip = 0, port = 0;

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	int r = NS[0]->sockname(c, 1, &ip, &port);
	CHECK(r == 0 && ip == inst_ip(1) && port == 8080,
	      "getpeername after a completed nonblocking connect: %d %x:%u", r,
	      ip, port);
	return 0;
}

/* SO_KEEPALIVE set before connect() is kept for the connection */
static int t_keepalive_before_connect(void)
{
	int s = NS[0]->socket(K_SOCK_STREAM), on = 1, v = 0;
	unsigned len = sizeof(v);

	CHECK(NS[0]->setsockopt(s, K_SOL_SOCKET, K_SO_KEEPALIVE, &on,
				sizeof(on)) == 0,
	      "setsockopt");
	int ls = NS[1]->socket(K_SOCK_STREAM);
	NS[1]->bind(ls, 0, 8080);
	NS[1]->listen(ls, 4);
	NS[0]->set_nonblock(s, 1);
	NS[0]->connect(s, inst_ip(1), 8080);
	CHECK(wait_established(0, s, 5 * SEC) == 0, "connect");
	CHECK(NS[0]->getsockopt(s, K_SOL_SOCKET, K_SO_KEEPALIVE, &v, &len) ==
			      0 &&
		      v,
	      "SO_KEEPALIVE set before connect reads back %d", v);
	return 0;
}

static int t_keepalive_inherit(void)
{
	int ls = NS[1]->socket(K_SOCK_STREAM), on = 1, v = 0;
	unsigned len = sizeof(v);

	NS[1]->set_nonblock(ls, 1);
	CHECK(NS[1]->setsockopt(ls, K_SOL_SOCKET, K_SO_KEEPALIVE, &on,
				sizeof(on)) == 0,
	      "setsockopt");
	NS[1]->bind(ls, 0, 8080);
	NS[1]->listen(ls, 4);
	int c = tcp_connect_nb(0, inst_ip(1), 8080);
	CHECK(wait_established(0, c, 5 * SEC) == 0, "connect");
	sim_run_for(100 * MS);
	int a = NS[1]->accept(ls, NULL, NULL);
	CHECK(a >= 0, "accept %d", a);
	CHECK(NS[1]->getsockopt(a, K_SOL_SOCKET, K_SO_KEEPALIVE, &v, &len) ==
			      0 &&
		      v,
	      "accepted socket did not inherit SO_KEEPALIVE (%d)", v);
	return 0;
}

static int t_tcp_cork(void)
{
	int c, s, on = 1;

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	int r = NS[0]->setsockopt(c, K_IPPROTO_TCP, K_TCP_CORK, &on,
				  sizeof(on));
	CHECK(r == 0, "setsockopt(TCP_CORK) %d", r);
	return 0;
}

/* The server sends its answer and closes while the client half-closes:
 * the client's connection ends in TIME_WAIT with the answer still unread,
 * and the answer must still be readable.  TIME_WAIT itself is held by a
 * TIME_WAIT sock (the 4-tuple, the sequence numbers, a timer): the
 * socket's own connection reports CLOSED from then on, its receive queue
 * intact, as it does on the common stack. */
static int t_timewait_unread(void)
{
	int c, s;
	uint8_t *buf = malloc(65536);
	struct path_cfg p = { 0 };

	p.delay_ms = 10;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	/* what fits in the initial window: all in flight at once, before
	 * the client's FIN arrives */
	pat_fill(6, 0, buf, 14000);
	int r = NS[1]->send(s, buf, 14000, 0);
	CHECK(r == 14000, "server send %d", r);
	NS[1]->close(s);
	NS[0]->shutdown(c, K_SHUT_WR);
	sim_run_for(5 * SEC);
	CHECK(NS[0]->tcp_state(c) == TS_CLOSED, "client state %d",
	      NS[0]->tcp_state(c));
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 1, "no TIME_WAIT sock");
	unsigned long long got = 0;
	for (;;) {
		r = NS[0]->recv(c, buf, 65536, 0);
		if (r <= 0)
			break;
		CHECK(!pat_check(6, got, buf, (size_t)r), "corrupt");
		got += (unsigned)r;
	}
	CHECK(got == 14000, "read %llu of 14000 bytes in TIME_WAIT", got);
	free(buf);
	return 0;
}

static int t_fionread_timewait(void)
{
	int c, s, v = -1;

	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	NS[1]->close(s);
	NS[0]->shutdown(c, K_SHUT_WR);
	sim_run_for(2 * SEC);
	CHECK(NS[0]->tcp_state(c) == TS_CLOSED, "state %d",
	      NS[0]->tcp_state(c));
	CHECK(NS[0]->counter(NSC_TCP_CONNS) == 1, "no TIME_WAIT sock");
	int r = NS[0]->ioctl(c, K_FIONREAD, &v);
	CHECK(r == 0 && v == 0, "FIONREAD in TIME_WAIT: %d, %d", r, v);
	return 0;
}

/* a slow reader makes the receive buffer grow: no slab call with
 * interrupts off on the way */
static int t_rx_grow_irqoff(void)
{
	int c, s;
	struct path_cfg p = { 0 };
	uint8_t *buf = malloc(65536);

	p.delay_ms = 20;
	link_set_both(&p);
	CHECK(tcp_pair(0, 1, 8080, &c, &s, NULL, 5 * SEC) == 0, "pair");
	unsigned long long sent = 0, got = 0;
	for (int i = 0; i < 2000; i++) {
		pat_fill(8, sent, buf, 65536);
		int r = NS[0]->send(c, buf, 65536, 0);
		if (r > 0)
			sent += (unsigned)r;
		sim_run_for(20 * MS);
		if (i % 10 == 0) { /* read a little, rarely */
			r = NS[1]->recv(s, buf, 20000, 0);
			if (r > 0)
				got += (unsigned)r;
		}
	}
	unsigned long long v = NS[1]->counter(NSC_SLAB_IRQOFF) +
			       NS[0]->counter(NSC_SLAB_IRQOFF);
	free(buf);
	CHECK(v == 0, "%llu slab calls with interrupts off", v);
	return 0;
}

/* An ICMP error quoting a sequence number outside snd_una..snd_nxt is
 * ignored (RFC 5927): it cannot belong to this connection.  In SYN_SENT,
 * where a matching error is fatal, the connection must survive it. */
static int t_icmp_out_of_window(void)
{
	uint8_t f[1600], q[28], m0[6];
	unsigned char m1[6];
	struct tcpseg sg;

	my_mac(0, m0);
	inst_mac(1, m1);
	NS[0]->arp_add(inst_ip(1), m1);
	cap_start(0, 1); /* black hole: the connection stays in SYN_SENT */
	int c = tcp_connect_nb(0, inst_ip(1), 8080);
	unsigned n = cap_next_proto(6, f, sizeof(f), 100 * MS);
	CHECK(n && !pkt_parse_tcp(f, n, &sg) && (sg.flags & F_SYN), "no SYN");
	memcpy(q, f + 14, 28);
	uint32_t bad = sg.seq + 100000;
	q[24] = (uint8_t)(bad >> 24);
	q[25] = (uint8_t)(bad >> 16);
	q[26] = (uint8_t)(bad >> 8);
	q[27] = (uint8_t)bad;
	for (int code = 0; code <= 3; code++) {
		n = pkt_icmp_err(f, m0, router_mac, ROUTER_IP, inst_ip(0), 3,
				 (uint8_t)code, 0, q, sizeof(q));
		NS[0]->rx(f, n);
	}
	sim_run_for(100 * MS);
	int err = so_error(0, c);
	CHECK(NS[0]->tcp_state(c) == TS_SYN_SENT && err == 0,
	      "an out-of-window ICMP error hit the connection (state %d, "
	      "SO_ERROR %d)",
	      NS[0]->tcp_state(c), err);
	return 0;
}

/* recv(MSG_TRUNC) on a datagram socket returns the datagram's real length
 * even when the buffer is shorter */
static int t_udp_msg_trunc(void)
{
	int a = NS[0]->socket(K_SOCK_DGRAM), b = NS[1]->socket(K_SOCK_DGRAM);
	uint8_t buf[1000], rb[100];

	NS[1]->set_nonblock(b, 1);
	CHECK(NS[1]->bind(b, 0, 5353) == 0, "bind");
	pat_fill(1, 0, buf, sizeof(buf));
	CHECK(NS[0]->sendto(a, buf, sizeof(buf), 0, inst_ip(1), 5353) == 1000,
	      "sendto");
	sim_run_for(50 * MS);
	int r = NS[1]->recvfrom(b, rb, sizeof(rb), K_MSG_TRUNC, NULL, NULL);
	CHECK(r == 1000, "recv(MSG_TRUNC) into 100 bytes returned %d", r);
	CHECK(!pat_check(1, 0, rb, sizeof(rb)), "the first 100 bytes");
	r = NS[1]->recvfrom(b, rb, sizeof(rb), 0, NULL, NULL);
	CHECK(r == -EAGAIN, "the datagram was not consumed: %d", r);
	return 0;
}

#define T(n, f, ...) { .name = n, .group = "unit", .fn = f, __VA_ARGS__ }
const struct test tests_unit[] = {
	T("arp-reply", t_arp_reply, .ninst = 1),
	T("arp-resolve", t_arp_resolve, .ninst = 1),
	T("icmp-echo-reply", t_icmp_echo_reply, .ninst = 1),
	T("ip-bad-header-csum", t_ip_bad_header_csum, .ninst = 1),
	T("ip-truncated", t_ip_truncated, .ninst = 1),
	T("ip-reassembly", t_ip_reassembly, .ninst = 1),
	T("ip-totlen-below-ihl", t_ip_totlen_below_ihl, .ninst = 1),
	T("seq-wrap-0", t_seq_wrap_0),
	T("seq-wrap-1", t_seq_wrap_1),
	T("simultaneous-open", t_simultaneous_open),
	T("oob-checksum", t_oob_checksum),
	T("send-oob", t_send_oob),
	T("time-wait-60s", t_time_wait_60s),
	T("syn-rto", t_syn_rto),
	T("icmp-soft-error", t_icmp_soft_error),
	T("icmp-out-of-window", t_icmp_out_of_window),
	T("udp-msg-trunc", t_udp_msg_trunc),
	T("second-connect", t_second_connect),
	T("getpeername-nonblock", t_getpeername_nonblock),
	T("keepalive-before-connect", t_keepalive_before_connect),
	T("keepalive-inherit", t_keepalive_inherit),
	T("tcp-cork", t_tcp_cork),
	T("timewait-unread", t_timewait_unread),
	T("fionread-timewait", t_fionread_timewait),
	T("rx-grow-irqoff", t_rx_grow_irqoff),
	{ 0 },
};
