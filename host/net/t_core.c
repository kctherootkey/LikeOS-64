/* Tests of the network core (wave 3): skb, the softirq loop, the timer
 * wheel, FIB/dst and path-MTU exceptions, NAPI and GRO, the device
 * transmit queue, and the neighbour (ARP) state machine.
 *
 * The probes that need the stack's internals run inside instance 0
 * (host/net/kt_core.c, called as n0_kt_*); the neighbour tests drive
 * instance 0 with crafted frames like t_unit.c does.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "tests.h"
#include "pkt.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* instance 0's kernel-side probes (host/net/kt_core.c) */
extern char n0_kt_msg[256];
int n0_kt_skb_basic(void);
int n0_kt_skb_clone(void);
int n0_kt_skb_fit(void);
int n0_kt_skb_queue(void);
int n0_kt_skb_csum(void);
int n0_kt_skb_pool(void);
int n0_kt_skb_pool_release(void);
int n0_kt_skb_dyn_slots(void);
int n0_kt_softirq_loop(void);
int n0_kt_timer_arm(void);
int n0_kt_timer_check(void);
int n0_kt_fib_lpm(void);
int n0_kt_fib_dst(void);
int n0_kt_fib_exc_set(void);
int n0_kt_fib_exc_expired(void);
int n0_kt_napi_budget(void);
int n0_kt_gro(void);
int n0_kt_xmit_setup(void);
int n0_kt_xmit_paths(void);
int n0_kt_xmit_stuck_begin(void);
int n0_kt_xmit_stuck_check(void);
int n0_kt_xmit_calls(void);
int n0_kt_xmit_bounded(void);
int n0_kt_if_events(void);
int n0_kt_route_oif(void);
int n0_kt_exc_scope(void);
int n0_kt_ts_math(void);
int n0_kt_neigh_state(unsigned ip, unsigned char *mac_out);
void n0_kt_neigh_confirm(unsigned ip);
unsigned n0_kt_neigh_entries(void);
unsigned long long n0_kt_arp_conflicts(void);
unsigned long long n0_kt_neigh_stat(int which);
int n0_kt_neigh_fill(int count, int hold, int base);
void n0_kt_neigh_unhold(void);
void n0_kt_neigh_gc_now(void);
void n1_kt_gro_enable(int on);
void n0_kt_gro_enable(int on);
void n0_kt_rx_batch(const unsigned char *buf, const unsigned *lens, int n);
void n1_kt_rx_batch(const unsigned char *buf, const unsigned *lens, int n);
unsigned long long n1_kt_gro_merged(void);

#define KRUN(fn)                                                         \
	do {                                                             \
		int line_ = fn();                                        \
		CHECK(line_ == 0, "%s: kt_core.c:%d: %s", #fn, line_,    \
		      n0_kt_msg);                                        \
	} while (0)

// NUD states (kernel/net/neighbour.h)
#define N_INCOMPLETE 0x01
#define N_REACHABLE 0x02
#define N_STALE 0x04
#define N_DELAY 0x08
#define N_PROBE 0x10
#define N_FAILED 0x20
#define N_PERMANENT 0x80

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t zero6[6] = { 0 };
static const uint8_t hostx_mac[6] = { 0x02, 0, 0, 0, 0, 0x09 };
static const uint8_t hostx_mac2[6] = { 0x02, 0, 0, 0, 0, 0x19 };
static const uint8_t hostx_mac3[6] = { 0x02, 0, 0, 0, 0, 0x29 };
#define HOSTX_IP ((10u << 24) | 9)
#define HOSTY_IP ((10u << 24) | 10)

static void my_mac(uint8_t m[6])
{
	unsigned char t[6];

	inst_mac(0, t);
	memcpy(m, t, 6);
}

/* ---- the probes that run inside the instance ---- */

static int t_skb_basic(void)
{
	KRUN(n0_kt_skb_basic);
	return 0;
}

static int t_skb_clone(void)
{
	KRUN(n0_kt_skb_clone);
	return 0;
}

static int t_skb_fit(void)
{
	KRUN(n0_kt_skb_fit);
	return 0;
}

static int t_skb_queue(void)
{
	KRUN(n0_kt_skb_queue);
	return 0;
}

static int t_skb_csum(void)
{
	KRUN(n0_kt_skb_csum);
	return 0;
}

static int t_skb_pool(void)
{
	KRUN(n0_kt_skb_pool);
	KRUN(n0_kt_skb_pool_release);
	CHECK(n0_kt_skb_dyn_slots() > 0, "the reserve went at once");
	sim_run_for(12 * SEC);
	CHECK(n0_kt_skb_dyn_slots() == 0,
	      "dynamic memory not given back after the pressure: %d slots",
	      n0_kt_skb_dyn_slots());
	return 0;
}

static int t_softirq_loop(void)
{
	KRUN(n0_kt_softirq_loop);
	return 0;
}

static int t_timer_wheel(void)
{
	KRUN(n0_kt_timer_arm);
	sim_run_for(4 * SEC); /* 800 ticks */
	KRUN(n0_kt_timer_check);
	return 0;
}

static int t_fib_lpm(void)
{
	KRUN(n0_kt_fib_lpm);
	return 0;
}

static int t_fib_dst(void)
{
	KRUN(n0_kt_fib_dst);
	return 0;
}

static int t_pmtu_expiry(void)
{
	KRUN(n0_kt_fib_exc_set);
	sim_run_for(599 * SEC);
	CHECK(n0_kt_fib_exc_set() != 0, "the exception vanished before 600 s");
	sim_run_for(2 * SEC);
	KRUN(n0_kt_fib_exc_expired);
	return 0;
}

static int t_napi_budget(void)
{
	KRUN(n0_kt_napi_budget);
	return 0;
}

static int t_gro(void)
{
	KRUN(n0_kt_gro);
	return 0;
}

static int t_xmit(void)
{
	KRUN(n0_kt_xmit_setup);
	KRUN(n0_kt_xmit_paths);
	return 0;
}

static int t_xmit_stuck(void)
{
	KRUN(n0_kt_xmit_setup);
	KRUN(n0_kt_xmit_stuck_begin);
	sim_run_for(300 * MS);
	KRUN(n0_kt_xmit_stuck_check);
	/* refusals are retried a bounded number of times, never spun on */
	CHECK(n0_kt_xmit_calls() < 400, "%d driver calls in 300 ms",
	      n0_kt_xmit_calls());
	return 0;
}

/* One process-context send hands at most one quota of frames to the
 * driver, also while others keep the queue filled. */
static int t_xmit_bounded(void)
{
	KRUN(n0_kt_xmit_setup);
	KRUN(n0_kt_xmit_bounded);
	return 0;
}

/* SIOCSIFADDR/NETMASK/MTU/FLAGS and the carrier raise their events. */
static int t_if_events(void)
{
	KRUN(n0_kt_if_events);
	CHECK(NS[0]->ping(inst_ip(0), 1, 1, "x", 1) >= 0, "the stack is alive");
	return 0;
}

static int t_route_oif(void)
{
	KRUN(n0_kt_xmit_setup);
	KRUN(n0_kt_route_oif);
	return 0;
}

static int t_exc_scope(void)
{
	KRUN(n0_kt_exc_scope);
	return 0;
}

static int t_ts_math(void)
{
	KRUN(n0_kt_ts_math);
	return 0;
}

/* ---- neighbours, driven with frames ---- */

static int is_arp(const uint8_t *f, unsigned n, int op)
{
	return n >= 42 && f[12] == 0x08 && f[13] == 0x06 && f[21] == op;
}

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* the next ARP request instance 0 sends within `wait'; 0 if none */
static unsigned next_request(uint8_t *f, simtime_t wait, simtime_t *when)
{
	simtime_t end = now_ns() + wait;

	for (;;) {
		simtime_t w = now_ns() < end ? end - now_ns() : 0;
		unsigned n = cap_next(f, 1600, w, when);

		if (!n)
			return 0;
		if (is_arp(f, n, 1))
			return n;
	}
}

static int send_udp(int s, uint32_t ip, unsigned len)
{
	static uint8_t buf[2000];

	memset(buf, 0x33, len);
	return NS[0]->sendto(s, buf, len, 0, ip, 7000);
}

static void inject_arp(int op, const uint8_t *smac, uint32_t sip,
		       const uint8_t *tmac, uint32_t tip, const uint8_t *dmac)
{
	uint8_t f[100];
	unsigned n = pkt_arp(f, op, smac, sip, tmac, tip, dmac);

	NS[0]->rx(f, n);
}

static int t_neigh_incomplete_failed(void)
{
	uint8_t f[1600], m0[6];
	simtime_t t0, when;
	int s = NS[0]->socket(K_SOCK_DGRAM);

	my_mac(m0);
	cap_start(0, 1);
	t0 = now_ns();
	CHECK(send_udp(s, HOSTX_IP, 100) == 100, "sendto");
	CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) == N_INCOMPLETE, "state %d",
	      n0_kt_neigh_state(HOSTX_IP, NULL));
	for (int i = 0; i < 3; i++) {
		CHECK(next_request(f, 1500 * MS, &when), "request %d", i);
		CHECK(!memcmp(f, bcast, 6), "request %d not broadcast", i);
		CHECK(rd32(f + 38) == HOSTX_IP && rd32(f + 28) == inst_ip(0),
		      "request %d: target/sender", i);
		CHECK(when >= t0 + (simtime_t)i * SEC &&
			      when <= t0 + (simtime_t)i * SEC + 20 * MS,
		      "request %d at %.3f s", i, (double)(when - t0) / SEC);
	}
	CHECK(!next_request(f, 2 * SEC, &when), "a 4th request at %.3f s",
	      (double)(when - t0) / SEC);
	CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) == N_FAILED, "state %d",
	      n0_kt_neigh_state(HOSTX_IP, NULL));
	/* the next packet starts over */
	CHECK(send_udp(s, HOSTX_IP, 100) == 100, "sendto");
	CHECK(next_request(f, 100 * MS, &when), "no new resolution");
	return 0;
}

/* resolve HOSTX: request, reply; the queued datagram goes out */
static int resolve_hostx(int s)
{
	uint8_t f[1600], m0[6];
	simtime_t when;

	my_mac(m0);
	CHECK(send_udp(s, HOSTX_IP, 100) == 100, "sendto");
	CHECK(next_request(f, 100 * MS, &when), "no request");
	inject_arp(2, hostx_mac, HOSTX_IP, m0, inst_ip(0), m0);
	CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) == N_REACHABLE,
	      "after the reply: %d", n0_kt_neigh_state(HOSTX_IP, NULL));
	CHECK(cap_next_proto(17, f, sizeof(f), 100 * MS), "datagram not sent");
	CHECK(!memcmp(f, hostx_mac, 6), "to the wrong MAC");
	return 0;
}

static int t_neigh_stale_delay_probe(void)
{
	uint8_t f[1600];
	simtime_t t0, when;
	int s = NS[0]->socket(K_SOCK_DGRAM);

	cap_start(0, 1);
	if (resolve_hostx(s))
		return 1;
	/* reachable_time is drawn from 15..45 s */
	sim_run_for(46 * SEC);
	CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) == N_STALE, "after 46 s: %d",
	      n0_kt_neigh_state(HOSTX_IP, NULL));
	/* a packet goes out at once with the cached address -> DELAY */
	CHECK(send_udp(s, HOSTX_IP, 100) == 100, "sendto");
	CHECK(cap_next_proto(17, f, sizeof(f), 50 * MS) && !memcmp(f, hostx_mac, 6),
	      "a STALE entry did not send");
	CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) == N_DELAY, "state %d",
	      n0_kt_neigh_state(HOSTX_IP, NULL));
	t0 = now_ns();
	/* no confirmation: PROBE after 5 s, unicast requests */
	for (int i = 0; i < 3; i++) {
		CHECK(next_request(f, 6 * SEC, &when), "probe %d", i);
		CHECK(!memcmp(f, hostx_mac, 6), "probe %d not unicast", i);
		CHECK(when >= t0 + 5 * SEC + (simtime_t)i * SEC - 20 * MS &&
			      when <= t0 + 5 * SEC + (simtime_t)i * SEC + 20 * MS,
		      "probe %d at %.3f s", i, (double)(when - t0) / SEC);
		if (i == 0)
			CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) == N_PROBE,
			      "state %d", n0_kt_neigh_state(HOSTX_IP, NULL));
	}
	sim_run_for(1100 * MS);
	CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) == N_FAILED, "state %d",
	      n0_kt_neigh_state(HOSTX_IP, NULL));
	return 0;
}

static int t_neigh_confirm(void)
{
	uint8_t f[1600];
	simtime_t when;
	int s = NS[0]->socket(K_SOCK_DGRAM);

	cap_start(0, 1);
	if (resolve_hostx(s))
		return 1;
	sim_run_for(46 * SEC);
	CHECK(send_udp(s, HOSTX_IP, 100) == 100, "sendto");
	CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) == N_DELAY, "not DELAY");
	sim_run_for(2 * SEC);
	n0_kt_neigh_confirm(HOSTX_IP); /* what a TCP ACK does */
	CHECK(!next_request(f, 4 * SEC, &when), "probed although confirmed");
	CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) == N_REACHABLE, "state %d",
	      n0_kt_neigh_state(HOSTX_IP, NULL));
	return 0;
}

static int t_arp_no_ip_learning(void)
{
	uint8_t f[1600], m0[6], data[64];
	unsigned n;

	my_mac(m0);
	cap_start(0, 1);
	/* an echo request from a host we never resolved */
	memset(data, 1, sizeof(data));
	n = pkt_icmp_echo(f, m0, hostx_mac, HOSTX_IP, inst_ip(0), 8, 0x55, 1,
			  data, sizeof(data));
	NS[0]->rx(f, n);
	CHECK(n0_kt_neigh_state(HOSTX_IP, NULL) != N_STALE &&
		      n0_kt_neigh_state(HOSTX_IP, NULL) != N_REACHABLE,
	      "learned from an IP packet");
	/* the reply waits for resolution */
	CHECK(next_request(f, 100 * MS, NULL), "no ARP request for the reply");
	/* an unsolicited reply about a host we do not know: ignored */
	inject_arp(2, hostx_mac2, HOSTY_IP, m0, inst_ip(0), m0);
	CHECK(n0_kt_neigh_state(HOSTY_IP, NULL) == -1,
	      "unsolicited reply created an entry");
	return 0;
}

static int t_arp_request_learns(void)
{
	uint8_t f[1600], m0[6], mac[6];

	my_mac(m0);
	cap_start(0, 1);
	inject_arp(1, hostx_mac, HOSTX_IP, zero6, inst_ip(0), bcast);
	CHECK(n0_kt_neigh_state(HOSTX_IP, mac) == N_STALE &&
		      !memcmp(mac, hostx_mac, 6),
	      "a request for us did not create STALE: %d",
	      n0_kt_neigh_state(HOSTX_IP, NULL));
	CHECK(cap_next_proto(0x806, f, sizeof(f), 50 * MS) && is_arp(f, 42, 2),
	      "no reply");
	/* a request for someone else from another host: nothing */
	inject_arp(1, hostx_mac2, HOSTY_IP, zero6, inst_ip(0) + 7, bcast);
	CHECK(n0_kt_neigh_state(HOSTY_IP, NULL) == -1,
	      "learned from a request not for us");
	return 0;
}

static int t_arp_locktime_garp(void)
{
	uint8_t m0[6], mac[6];
	int s = NS[0]->socket(K_SOCK_DGRAM);

	my_mac(m0);
	cap_start(0, 1);
	if (resolve_hostx(s))
		return 1;
	/* a second reply with another address within the lock time */
	sim_run_for(300 * MS);
	inject_arp(2, hostx_mac2, HOSTX_IP, m0, inst_ip(0), m0);
	n0_kt_neigh_state(HOSTX_IP, mac);
	CHECK(!memcmp(mac, hostx_mac, 6), "overridden within the lock time");
	/* after it */
	sim_run_for(1 * SEC);
	inject_arp(2, hostx_mac2, HOSTX_IP, m0, inst_ip(0), m0);
	CHECK(n0_kt_neigh_state(HOSTX_IP, mac) == N_REACHABLE &&
		      !memcmp(mac, hostx_mac2, 6),
	      "not overridden after the lock time");
	/* a gratuitous announcement replaces it at once */
	inject_arp(1, hostx_mac3, HOSTX_IP, zero6, HOSTX_IP, bcast);
	CHECK(n0_kt_neigh_state(HOSTX_IP, mac) == N_STALE &&
		      !memcmp(mac, hostx_mac3, 6),
	      "gratuitous ARP ignored: state %d",
	      n0_kt_neigh_state(HOSTX_IP, NULL));
	return 0;
}

static int t_arp_conflict(void)
{
	uint8_t f[1600];
	unsigned n;
	unsigned long long c0 = n0_kt_arp_conflicts();

	cap_start(0, 1);
	/* someone else claims our address */
	inject_arp(1, hostx_mac, inst_ip(0), zero6, inst_ip(0) + 5, bcast);
	CHECK(n0_kt_arp_conflicts() == c0 + 1, "not counted");
	n = cap_next_proto(0x806, f, sizeof(f), 50 * MS);
	CHECK(n && !memcmp(f, bcast, 6) && rd32(f + 28) == inst_ip(0) &&
		      rd32(f + 38) == inst_ip(0),
	      "the address was not defended");
	CHECK(n0_kt_neigh_state(inst_ip(0), NULL) == -1,
	      "learned our own address");
	/* within the defend interval: counted, not announced again */
	inject_arp(1, hostx_mac, inst_ip(0), zero6, inst_ip(0) + 5, bcast);
	CHECK(n0_kt_arp_conflicts() == c0 + 2, "second not counted");
	CHECK(!cap_next_proto(0x806, f, sizeof(f), 50 * MS),
	      "defended twice within 10 s");
	return 0;
}

static int t_neigh_gc_periodic(void)
{
	unsigned e0 = n0_kt_neigh_entries();

	CHECK(n0_kt_neigh_fill(300, 0, 0) == 300, "fill");
	CHECK(n0_kt_neigh_entries() >= e0 + 300, "entries %u",
	      n0_kt_neigh_entries());
	sim_run_for(30 * SEC);
	CHECK(n0_kt_neigh_entries() >= e0 + 300, "collected before gc_staletime");
	/* unused for 60 s: the periodic collector (every 15 s) takes them */
	sim_run_for(50 * SEC);
	CHECK(n0_kt_neigh_entries() <= e0, "after 80 s: %u entries",
	      n0_kt_neigh_entries());
	return 0;
}

static int t_neigh_gc_forced(void)
{
	/* fill to gc_thresh3 with entries older than 5 s */
	CHECK(n0_kt_neigh_fill(1000, 0, 0) > 900, "fill");
	sim_run_for(6 * SEC);
	unsigned long long g0 = n0_kt_neigh_stat(1);
	CHECK(n0_kt_neigh_fill(100, 0, 2000) == 100,
	      "creation above gc_thresh2 failed");
	CHECK(n0_kt_neigh_stat(1) > g0, "no forced collection");
	CHECK(n0_kt_neigh_entries() <= 700, "entries %u",
	      n0_kt_neigh_entries());
	return 0;
}

static int t_neigh_table_full(void)
{
	/* every entry in use: nothing can be collected, creation fails */
	int made = n0_kt_neigh_fill(1100, 1, 0);
	unsigned long long full = n0_kt_neigh_stat(2);

	CHECK(made <= 1024 && made > 1000, "made %d", made);
	CHECK(full > 0, "no table-full count");
	n0_kt_neigh_unhold();
	sim_run_for(6 * SEC);
	CHECK(n0_kt_neigh_fill(10, 0, 3000) == 10, "no recovery after release");
	return 0;
}

static int t_neigh_queue_bytes(void)
{
	uint8_t f[1600], m0[6];
	int s = NS[0]->socket(K_SOCK_DGRAM), got = 0;
	unsigned long long d0 = n0_kt_neigh_stat(0);

	my_mac(m0);
	cap_start(0, 1);
	/* 150 frames of ~1.4 KB: more than the 212992-byte queue holds */
	for (int i = 0; i < 150; i++)
		CHECK(send_udp(s, HOSTX_IP, 1400) == 1400, "sendto %d", i);
	CHECK(n0_kt_neigh_stat(0) > d0, "nothing dropped from the queue");
	inject_arp(2, hostx_mac, HOSTX_IP, m0, inst_ip(0), m0);
	while (cap_next_proto(17, f, sizeof(f), 20 * MS))
		got++;
	CHECK(got >= 90 && got < 150, "%d of 150 datagrams sent", got);
	return 0;
}

static int t_neigh_connect_unreachable(void)
{
	uint8_t f[1600];
	int c;
	simtime_t t0 = now_ns();

	cap_start(0, 1);
	c = tcp_connect_nb(0, HOSTX_IP, 80);
	CHECK(c >= 0, "connect");
	CHECK(next_request(f, 100 * MS, NULL), "no ARP request");
	sim_run_for(3500 * MS);
	int e = so_error(0, c); /* reading SO_ERROR clears it */
	CHECK(e == EHOSTUNREACH, "SO_ERROR %d after %.1f s", e,
	      (double)(now_ns() - t0) / SEC);
	return 0;
}

/* ---- GRO against the real TCP receive path ----
 * Frames from instance 0 are held back for 1 ms and handed to instance 1
 * as one burst, so its receive softirq sees several segments of a flow at
 * once and GRO merges them. */
#define GB_MAX 256
static unsigned char gb_buf[GB_MAX * 1600];
static unsigned gb_len[GB_MAX], gb_off;
static int gb_n, gb_armed, gb_strip_psh;

static void gb_flush(void *arg, long a)
{
	(void)arg;
	(void)a;
	gb_armed = 0;
	if (!gb_n)
		return;
	n1_kt_rx_batch(gb_buf, gb_len, gb_n);
	gb_n = 0;
	gb_off = 0;
}

static int gb_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	(void)ctx;
	if (inst != 0 || len > 1600)
		return 0;
	if (gb_n == GB_MAX)
		gb_flush(NULL, 0);
	memcpy(gb_buf + gb_off, f, len);
	/* This stack pushes every data segment, and a pushed segment ends a
	 * GRO run; clear PSH in transit (checksums fixed) so the merging,
	 * and TCP's handling of merged segments, gets exercised. */
	if (gb_strip_psh && len >= 54 && f[12] == 0x08 && f[13] == 0x00 &&
	    f[23] == 6 && (f[14] & 0x0F) == 5) {
		gb_buf[gb_off + 14 + 20 + 13] &= (uint8_t)~0x08;
		frame_fix_csums(gb_buf + gb_off, len);
	}
	gb_len[gb_n++] = len;
	gb_off += len;
	if (!gb_armed) {
		gb_armed = 1;
		sim_after(1 * MS, gb_flush, NULL, 0);
	}
	return 1;
}

static int gro_interop(int strip_psh)
{
	int c, s, l;
	long long got;

	CHECK(tcp_pair(0, 1, 5001, &c, &s, &l, 5 * SEC) == 0, "pair");
	n1_kt_gro_enable(1);
	gb_strip_psh = strip_psh;
	wire_set_tap(gb_tap, NULL);
	got = transfer(0, c, 1, s, 77, 4000000, 60 * SEC);
	wire_set_tap(NULL, NULL);
	gb_flush(NULL, 0);
	n1_kt_gro_enable(0);
	CHECK(got == 4000000, "moved %lld of 4000000 bytes", got);
	tlog("gro: %llu segments merged", n1_kt_gro_merged());
	if (strip_psh)
		CHECK(n1_kt_gro_merged() > 100, "only %llu segments merged",
		      n1_kt_gro_merged());
	return 0;
}

/* GRO on, the stack's own segments (all pushed: nothing merges). */
static int t_gro_interop(void)
{
	return gro_interop(0);
}

/* GRO on, unpushed segments: TCP receives merged segments of up to 64 KB. */
static int t_gro_interop_merged(void)
{
	return gro_interop(1);
}

/* Mutated TCP bursts through the GRO parser and merger (GRO on): bit
 * flips, header fields, truncation; checksums repaired half the time. */
static int t_gro_fuzz(void)
{
	static unsigned char batch[16 * 1600];
	unsigned lens[16];
	uint8_t m0[6], f[1600], data[1400];
	struct tcpseg sg;

	my_mac(m0);
	n0_kt_gro_enable(1);
	for (int it = 0; it < 4000; it++) {
		unsigned off = 0;
		uint32_t seq = (uint32_t)rnd64();
		int n = 1 + (int)rnd_below(16);

		for (int i = 0; i < n; i++) {
			unsigned plen = rnd_below(4) ? 1000 : rnd_below(1400);
			unsigned len;

			tcpseg_init(&sg);
			sg.sip = HOSTX_IP + rnd_below(3);
			sg.dip = inst_ip(0);
			sg.sport = (uint16_t)(5000 + rnd_below(3));
			sg.dport = 80;
			sg.seq = seq;
			sg.ack = 4242;
			sg.flags = (uint8_t)(rnd_below(8) ? 0x10 : rnd_below(64));
			sg.win = 1000;
			if (rnd_below(2)) {
				sg.ts = 1;
				sg.tsval = 7;
				sg.tsecr = 9;
			}
			memset(data, (int)i, plen);
			sg.data = data;
			sg.len = plen;
			len = pkt_tcp(f, m0, hostx_mac, &sg);
			seq += rnd_below(5) ? plen : rnd_below(3000);
			for (unsigned k = rnd_below(4); k; k--) {
				unsigned pos = rnd_below(len);

				f[pos] ^= (uint8_t)(1u << rnd_below(8));
			}
			if (rnd_below(8) == 0)
				len = rnd_below(len) + 1;
			if (rnd_below(2))
				frame_fix_csums(f, len);
			memcpy(batch + off, f, len);
			lens[i] = len;
			off += len;
		}
		n0_kt_rx_batch(batch, lens, n);
	}
	n0_kt_gro_enable(0);
	CHECK(NS[0]->ping(inst_ip(0), 1, 1, data, 8) >= 0, "the stack is alive");
	return 0;
}

#define T(n, f, ...) { .name = n, .group = "unit", .fn = f, __VA_ARGS__ }
const struct test tests_core[] = {
	T("core-skb-basic", t_skb_basic, .ninst = 1, .allow_warn = 1),
	T("core-skb-clone", t_skb_clone, .ninst = 1),
	T("core-skb-fit", t_skb_fit, .ninst = 1),
	T("core-skb-queue", t_skb_queue, .ninst = 1),
	T("core-skb-csum", t_skb_csum, .ninst = 1),
	T("core-skb-pool", t_skb_pool, .ninst = 1),
	T("core-softirq-loop", t_softirq_loop, .ninst = 1),
	T("core-timer-wheel", t_timer_wheel, .ninst = 1),
	T("core-fib-lpm", t_fib_lpm, .ninst = 1),
	T("core-fib-dst", t_fib_dst, .ninst = 1),
	T("core-pmtu-expiry", t_pmtu_expiry, .ninst = 1),
	T("core-napi-budget", t_napi_budget, .ninst = 1),
	T("core-gro", t_gro, .ninst = 1),
	T("core-gro-interop", t_gro_interop),
	T("core-gro-interop-merged", t_gro_interop_merged),
	T("core-gro-fuzz", t_gro_fuzz, .ninst = 1, .allow_warn = 1),
	T("core-xmit", t_xmit, .ninst = 1),
	T("core-xmit-stuck", t_xmit_stuck, .ninst = 1),
	T("core-xmit-bounded", t_xmit_bounded, .ninst = 1),
	T("core-if-events", t_if_events, .ninst = 1),
	T("core-route-oif", t_route_oif, .ninst = 1),
	T("core-pmtu-exception-scope", t_exc_scope, .ninst = 1),
	T("core-recvmmsg-timeout-math", t_ts_math, .ninst = 1),
	T("neigh-incomplete-failed", t_neigh_incomplete_failed, .ninst = 1),
	T("neigh-stale-delay-probe", t_neigh_stale_delay_probe, .ninst = 1),
	T("neigh-confirm", t_neigh_confirm, .ninst = 1),
	T("neigh-gc-periodic", t_neigh_gc_periodic, .ninst = 1),
	T("neigh-gc-forced", t_neigh_gc_forced, .ninst = 1),
	T("neigh-table-full", t_neigh_table_full, .ninst = 1),
	T("neigh-queue-bytes", t_neigh_queue_bytes, .ninst = 1),
	T("neigh-connect-unreachable", t_neigh_connect_unreachable, .ninst = 1),
	T("arp-no-ip-learning", t_arp_no_ip_learning, .ninst = 1),
	T("arp-request-learns", t_arp_request_learns, .ninst = 1),
	T("arp-locktime-garp", t_arp_locktime_garp, .ninst = 1),
	T("arp-conflict", t_arp_conflict, .ninst = 1),
	{ 0 }
};
