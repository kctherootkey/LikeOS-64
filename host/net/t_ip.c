/* Tests of the IPv4 layer (input checks, options, fragmentation and
 * reassembly, output), ICMP (replies, errors, rate limits, path MTU) and
 * UDP (lookup, reuseport, fan-out, receive buffer, cork, error queue).
 *
 * Crafted frames go into instance 0 from a made-up neighbour 10.0.0.9
 * (and 10.0.0.10); what instance 0 sends is captured.  The probes that
 * need the stack's internals (counters, recvmsg with ancillary data)
 * run inside the instance (host/net/kt_ip.c).
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "tests.h"
#include "pkt.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- the probes (host/net/kt_ip.c) ---- */
struct kt_msg {
	int ret;
	int flags;
	unsigned src_ip, src_port;
	int have_pktinfo, pkt_ifindex;
	unsigned pkt_addr, pkt_spec_dst;
	int have_ttl, ttl;
	int have_tos, tos;
	int have_err;
	unsigned ee_errno, ee_origin, ee_type, ee_code, ee_info, offender;
	unsigned controllen;
};
unsigned long long n0_kt_mib(const char *name);
unsigned long long n1_kt_mib(const char *name);
void n0_kt_ipfrag(unsigned *queues, unsigned long long *mem,
		  unsigned long long *evicted, unsigned long long *timeouts);
void n0_kt_ipfrag_thresh(unsigned long long high, unsigned long long low);
void n0_kt_icmp_rl_reset(void);
unsigned n0_kt_pmtu(unsigned daddr);
unsigned n0_kt_udp_hashed(void);
int n0_kt_recvmsg(int sock, void *buf, unsigned len, int flags,
		  struct kt_msg *out);
int n1_kt_recvmsg(int sock, void *buf, unsigned len, int flags,
		  struct kt_msg *out);
int n0_kt_sendmsg(int sock, const void *buf, unsigned len, int flags,
		  unsigned ip, unsigned port);
int n0_kt_raw_socket(int proto);
int n0_kt_disconnect(int sock);
int n0_kt_mc_join(int sock, unsigned group, unsigned ifaddr, int drop);
int n0_kt_rx_skb(const void *frame, unsigned len, int verified);
unsigned long long n0_kt_skb_inuse(void);
unsigned long long n0_kt_dev_stat(int which);
void n0_kt_dev_addr(unsigned ip);
int n0_kt_clone_send(unsigned daddr);

#define MIB(name) n0_kt_mib(name)

// kernel values the tests use
#define K_IPPROTO_IP 0
#define K_IP_TOS 1
#define K_IP_TTL 2
#define K_IP_PKTINFO 8
#define K_IP_MTU_DISCOVER 10
#define K_IP_RECVERR 11
#define K_IP_RECVTTL 12
#define K_IP_RECVTOS 13
#define K_IP_MTU 14
#define K_IP_MULTICAST_TTL 33
#define K_IP_MULTICAST_LOOP 34
#define K_SO_REUSEADDR 2
#define K_SO_BROADCAST 6
#define K_SO_REUSEPORT 15
#define K_SOL_UDP 17
#define K_UDP_CORK 1
#define K_MSG_MORE 0x8000
#define K_MSG_ERRQUEUE 0x2000
#define K_MSG_CTRUNC 0x08
#define PMTU_DONT 0
#define PMTU_WANT 1
#define PMTU_DO 2
#define PMTU_INTERFACE 4
#define PMTU_OMIT 5

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t hx_mac[6] = { 0x02, 0, 0, 0, 0, 0x09 };
static const uint8_t hy_mac[6] = { 0x02, 0, 0, 0, 0, 0x0a };
#define HX ((10u << 24) | 9)
#define HY ((10u << 24) | 10)
#define ME (inst_ip(0))
#define SUBNET_BCAST ((10u << 24) | 255)

static void put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static unsigned get16(const uint8_t *p)
{
	return (unsigned)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static void my_mac(int i, uint8_t m[6])
{
	unsigned char t[6];

	inst_mac(i, t);
	memcpy(m, t, 6);
}

/* An IPv4 frame from `smac' to instance 0's MAC (or `dmac'), with
 * options (a multiple of 4 bytes) and the given header fields. */
static unsigned ipf(uint8_t *out, const uint8_t *smac, const uint8_t *dmac,
		    uint32_t sip, uint32_t dip, uint8_t proto,
		    const uint8_t *l4, unsigned l4len, uint16_t id,
		    uint16_t frag, uint8_t ttl, uint8_t tos, const uint8_t *opt,
		    unsigned optlen)
{
	uint8_t m0[6];
	uint8_t *ip = out + 14;
	unsigned hl = 20 + optlen;

	my_mac(0, m0);
	memcpy(out, dmac ? dmac : m0, 6);
	memcpy(out + 6, smac, 6);
	out[12] = 8;
	out[13] = 0;
	ip[0] = (uint8_t)(0x40 | (hl / 4));
	ip[1] = tos;
	put16(ip + 2, hl + l4len);
	put16(ip + 4, id);
	put16(ip + 6, frag);
	ip[8] = ttl;
	ip[9] = proto;
	put16(ip + 10, 0);
	put32(ip + 12, sip);
	put32(ip + 16, dip);
	if (optlen)
		memcpy(ip + 20, opt, optlen);
	put16(ip + 10, inet_csum(ip, hl, 0));
	if (l4len)
		memcpy(ip + hl, l4, l4len);
	return 14 + hl + l4len;
}

static uint32_t pseudo(uint32_t sip, uint32_t dip, uint8_t proto, unsigned len)
{
	return (sip >> 16) + (sip & 0xffff) + (dip >> 16) + (dip & 0xffff) +
	       proto + len;
}

/* A UDP datagram (whole); csum: 0 computed, 1 zero, 2 wrong. */
static unsigned udpd(uint8_t *l4, uint32_t sip, uint32_t dip, unsigned sport,
		     unsigned dport, const uint8_t *data, unsigned len,
		     int csum)
{
	put16(l4, sport);
	put16(l4 + 2, dport);
	put16(l4 + 4, 8 + len);
	put16(l4 + 6, 0);
	if (len)
		memcpy(l4 + 8, data, len);
	if (csum == 0 || csum == 2) {
		uint16_t c = inet_csum(l4, 8 + len, pseudo(sip, dip, 17, 8 + len));

		if (csum == 2)
			c ^= 0x1234;
		put16(l4 + 6, c ? c : 0xffff);
	}
	return 8 + len;
}

/* An ICMP message (whole). */
static unsigned icmpm(uint8_t *l4, uint8_t type, uint8_t code, uint32_t rest,
		      const uint8_t *body, unsigned blen)
{
	l4[0] = type;
	l4[1] = code;
	put16(l4 + 2, 0);
	put32(l4 + 4, rest);
	if (blen)
		memcpy(l4 + 8, body, blen);
	put16(l4 + 2, inet_csum(l4, 8 + blen, 0));
	return 8 + blen;
}

static unsigned echo_from(uint8_t *f, uint32_t sip, uint32_t dip,
			  const uint8_t *dmac, uint16_t seq, const uint8_t *opt,
			  unsigned optlen)
{
	uint8_t l4[200], body[64];

	pat_fill(seq, 0, body, sizeof(body));
	unsigned n = icmpm(l4, 8, 0, (0x77u << 16) | seq, body, sizeof(body));
	return ipf(f, hx_mac, dmac, sip, dip, 1, l4, n, seq, 0, 64, 0, opt,
		   optlen);
}

/* Inject, then collect what instance 0 answered with protocol `proto'
 * within 100 ms (0: nothing). */
static unsigned rx_and_reply(const uint8_t *f, unsigned n, int proto,
			     uint8_t *out, unsigned cap)
{
	NS[0]->rx(f, n);
	return cap_next_proto(proto, out, cap, 100 * MS);
}

static void setup_x(void)
{
	NS[0]->arp_add(HX, hx_mac);
	NS[0]->arp_add(HY, hy_mac);
	n0_kt_icmp_rl_reset();
	cap_start(0, 1);
}

/* ================================================================== */
/* IPv4 input                                                          */
/* ================================================================== */

static int t_ip_header_checks(void)
{
	uint8_t f[1600], r[1600];
	unsigned n;
	unsigned long long e0;

	setup_x();
	// sane: answered
	n = echo_from(f, HX, ME, NULL, 1, NULL, 0);
	CHECK(rx_and_reply(f, n, 1, r, sizeof(r)), "no reply to a good echo");
	// version 6 in an IPv4 frame
	n = echo_from(f, HX, ME, NULL, 2, NULL, 0);
	f[14] = 0x65;
	put16(f + 24, 0);
	put16(f + 24, inet_csum(f + 14, 20, 0));
	e0 = MIB("IpInHdrErrors");
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered version 6");
	CHECK(MIB("IpInHdrErrors") == e0 + 1, "version: not counted");
	// header length 4 words
	n = echo_from(f, HX, ME, NULL, 3, NULL, 0);
	f[14] = 0x44;
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered ihl 4");
	CHECK(MIB("IpInHdrErrors") == e0 + 2, "ihl 4: not counted");
	// header length past the frame
	n = echo_from(f, HX, ME, NULL, 4, NULL, 0);
	f[14] = 0x4f;
	CHECK(!rx_and_reply(f, 14 + 40, 1, r, sizeof(r)), "answered ihl 15");
	CHECK(MIB("IpInHdrErrors") == e0 + 3, "ihl past frame: not counted");
	// bad header checksum
	n = echo_from(f, HX, ME, NULL, 5, NULL, 0);
	f[24] ^= 0x40;
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered a bad checksum");
	CHECK(MIB("IpInCsumErrors") == 1, "checksum: not counted");
	// total length beyond the frame: truncated
	n = echo_from(f, HX, ME, NULL, 6, NULL, 0);
	e0 = MIB("IpInTruncatedPkts");
	CHECK(!rx_and_reply(f, n - 10, 1, r, sizeof(r)), "answered truncated");
	CHECK(MIB("IpInTruncatedPkts") == e0 + 1, "truncated: not counted");
	// link padding after the datagram is cut off, not echoed
	n = echo_from(f, HX, ME, NULL, 7, NULL, 0);
	memset(f + n, 0xEE, 30);
	n = rx_and_reply(f, n + 30, 1, r, sizeof(r));
	CHECK(n == 14 + 20 + 8 + 64, "reply to a padded frame has %u bytes", n);
	CHECK(!frame_csum_bad(r, n), "reply checksum");
	return 0;
}

static int t_ip_martians(void)
{
	static const uint32_t srcs[] = {
		0x7F000001u, // loopback
		0xE0000005u, // multicast
		0xFFFFFFFFu, // limited broadcast
		0x00000001u, // 0.0.0.0/8
		0x0A000001u, // our own address
		0x0A0000FFu, // our subnet's broadcast
	};
	uint8_t f[1600], r[1600];
	unsigned n;
	unsigned long long m0;

	setup_x();
	m0 = MIB("IpInMartians");
	for (unsigned i = 0; i < sizeof(srcs) / sizeof(srcs[0]); i++) {
		n = echo_from(f, srcs[i], ME, NULL, (uint16_t)(10 + i), NULL, 0);
		CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)),
		      "answered martian source %08x", srcs[i]);
	}
	CHECK(MIB("IpInMartians") == m0 + 6, "martians counted %llu",
	      MIB("IpInMartians") - m0);
	// a loopback destination on the wire
	n = echo_from(f, HX, 0x7F000001u, NULL, 20, NULL, 0);
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered to 127.0.0.1");
	// not ours: no forwarding
	m0 = MIB("IpInAddrErrors");
	n = echo_from(f, HX, (10u << 24) | 77, NULL, 21, NULL, 0);
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered for 10.0.0.77");
	CHECK(MIB("IpInAddrErrors") == m0 + 1, "not-ours not counted");
	// a zero source to the limited broadcast is fine (DHCP)
	{
		int s = NS[0]->socket(K_SOCK_DGRAM);
		uint8_t l4[100], d[10] = "dhcpish";

		NS[0]->set_nonblock(s, 1);
		CHECK(NS[0]->bind(s, 0, 4444) == 0, "bind");
		n = udpd(l4, 0, 0xFFFFFFFFu, 68, 4444, d, 8, 0);
		n = ipf(f, hx_mac, bcast, 0, 0xFFFFFFFFu, 17, l4, n, 1, 0, 64,
			0, NULL, 0);
		NS[0]->rx(f, n);
		CHECK(NS[0]->recvfrom(s, r, 100, 0, NULL, NULL) == 8,
		      "0.0.0.0 -> 255.255.255.255 not delivered");
	}
	return 0;
}

static int t_ip_unknown_proto(void)
{
	uint8_t f[1600], r[1600], l4[40] = { 1, 2, 3, 4 };
	unsigned n;

	setup_x();
	n = ipf(f, hx_mac, NULL, HX, ME, 99, l4, 20, 5, 0, 64, 0, NULL, 0);
	n = rx_and_reply(f, n, 1, r, sizeof(r));
	CHECK(n, "no protocol unreachable");
	CHECK(r[34] == 3 && r[35] == 2, "ICMP %u/%u", r[34], r[35]);
	CHECK(get32(r + 14 + 16) == HX, "sent to %08x", get32(r + 30));
	CHECK(r[14 + 28 + 9] == 99, "does not quote the datagram");
	CHECK(MIB("IpInUnknownProtos") == 1, "not counted");
	// a raw socket of the protocol takes it: no error then
	{
		int rs = n0_kt_raw_socket(99);
		uint8_t buf[200];

		CHECK(rs >= 0, "raw socket %d", rs);
		NS[0]->set_nonblock(rs, 1);
		n = ipf(f, hx_mac, NULL, HX, ME, 99, l4, 20, 6, 0, 64, 0, NULL,
			0);
		CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)),
		      "protocol unreachable although a raw socket took it");
		int got = NS[0]->recvfrom(rs, buf, sizeof(buf), 0, NULL, NULL);
		CHECK(got == 40 && buf[0] == 0x45 && buf[9] == 99,
		      "raw socket got %d bytes", got);
		NS[0]->close(rs);
	}
	return 0;
}

static int t_ip_options(void)
{
	uint8_t f[1600], r[1600];
	unsigned n;
	struct {
		const char *what;
		uint8_t opt[40];
		unsigned len;
		int ok; // 1 answered, 0 parameter problem
		unsigned ptr; // expected pointer (offset in the header)
	} cases[] = {
		{ "NOPs", { 1, 1, 1, 0 }, 4, 1, 0 },
		{ "RR with room", { 7, 11, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 12, 1, 0 },
		{ "RR full", { 7, 7, 8, 1, 2, 3, 4, 0 }, 8, 1, 0 },
		{ "TS only", { 68, 12, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 12, 1, 0 },
		{ "RA", { 148, 4, 0, 0 }, 4, 1, 0 },
		{ "SEC", { 130, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 12, 1, 0 },
		{ "unknown option", { 25, 4, 1, 2 }, 4, 1, 0 },
		{ "length 1", { 7, 1, 0, 0 }, 4, 0, 20 },
		{ "length past the end", { 1, 1, 7, 9 }, 4, 0, 22 },
		{ "RR pointer 3", { 7, 7, 3, 0, 0, 0, 0, 0 }, 8, 0, 22 },
		{ "RR no room", { 7, 6, 4, 0, 0, 0, 0, 0 }, 8, 0, 22 },
		{ "two RRs", { 7, 3, 4, 7, 3, 4, 0, 0 }, 8, 0, 23 },
		{ "TS too short", { 68, 3, 5, 0 }, 4, 0, 21 },
		{ "TS pointer 4", { 68, 8, 4, 0, 0, 0, 0, 0 }, 8, 0, 22 },
		{ "TS overflow", { 68, 8, 9, 0xF0, 0, 0, 0, 0 }, 8, 0, 23 },
		{ "RA short", { 148, 3, 0, 0 }, 4, 0, 21 },
		{ "lone byte", { 1, 1, 1, 7 }, 4, 0, 23 },
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		setup_x(); // fresh rate-limit budget each time
		n = echo_from(f, HX, ME, NULL, (uint16_t)(30 + i), cases[i].opt,
			      cases[i].len);
		NS[0]->rx(f, n);
		n = cap_next_proto(1, r, sizeof(r), 100 * MS);
		CHECK(n, "%s: no answer at all", cases[i].what);
		if (cases[i].ok) {
			CHECK(r[34] == 0, "%s: answered with ICMP %u/%u",
			      cases[i].what, r[34], r[35]);
		} else {
			CHECK(r[34] == 12 && r[35] == 0,
			      "%s: ICMP %u/%u, not a parameter problem",
			      cases[i].what, r[34], r[35]);
			CHECK(r[38] == cases[i].ptr,
			      "%s: pointer %u, expected %u", cases[i].what,
			      r[38], cases[i].ptr);
			CHECK(!cap_next_proto(1, r, sizeof(r), 50 * MS),
			      "%s: the datagram was answered too",
			      cases[i].what);
		}
	}
	// parameter problems are rate-limited: six, then one a second
	setup_x();
	{
		uint8_t bad[4] = { 7, 1, 0, 0 };
		int got = 0;

		for (int i = 0; i < 20; i++) {
			n = echo_from(f, HX, ME, NULL, (uint16_t)(100 + i), bad,
				      4);
			NS[0]->rx(f, n);
		}
		while (cap_next_proto(1, r, sizeof(r), 50 * MS))
			got++;
		CHECK(got == 6, "%d parameter problems to one host at once",
		      got);
		CHECK(MIB("IcmpOutRateLimitHost") >= 14, "host limit counted %llu",
		      MIB("IcmpOutRateLimitHost"));
	}
	return 0;
}

static int t_ip_source_route(void)
{
	uint8_t f[1600], r[1600];
	uint8_t lsrr[8] = { 131, 7, 4, 10, 0, 0, 2, 0 };
	uint8_t ssrr[8] = { 137, 7, 4, 10, 0, 0, 2, 0 };
	unsigned n;

	setup_x();
	n = echo_from(f, HX, ME, NULL, 1, lsrr, 8);
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered an LSRR datagram");
	n = echo_from(f, HX, ME, NULL, 2, ssrr, 8);
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered an SSRR datagram");
	CHECK(MIB("IpInSrcRouted") == 2, "source-routed drops %llu",
	      MIB("IpInSrcRouted"));
	return 0;
}

/* ================================================================== */
/* Reassembly                                                          */
/* ================================================================== */

/* A UDP datagram of `len' bytes to instance 0 port `dport', cut into
 * fragments of `fsz' bytes (multiple of 8): fragment i of it into f. */
static uint8_t big_l4[70000];
static unsigned big_len;

static void big_udp(unsigned len, unsigned dport, unsigned seed)
{
	uint8_t *data = malloc(len);

	pat_fill(seed, 0, data, len);
	big_len = udpd(big_l4, HX, ME, 5000, dport, data, len, 0);
	free(data);
}

static unsigned big_frag(uint8_t *f, unsigned off, unsigned flen, uint16_t id,
			 int last)
{
	uint16_t frag = (uint16_t)(off / 8) | (last ? 0 : 0x2000);

	if (off + flen > big_len)
		flen = big_len - off;
	return ipf(f, hx_mac, NULL, HX, ME, 17, big_l4 + off, flen, id, frag,
		   64, 0, NULL, 0);
}

static int udp_sock(int inst, unsigned port)
{
	int s = NS[inst]->socket(K_SOCK_DGRAM);

	NS[inst]->set_nonblock(s, 1);
	if (port && NS[inst]->bind(s, 0, port) != 0)
		return -1;
	return s;
}

static int t_frag_dup_and_order(void)
{
	uint8_t f[2200], *rb = malloc(70000);
	int s;

	setup_x();
	s = udp_sock(0, 6000);
	CHECK(s >= 0, "bind");
	big_udp(4000, 6000, 3);
	// 4008 bytes: 0..1480, 1480..2960, 2960..4008; out of order with dups
	unsigned order[] = { 2960, 0, 0, 1480, 1480 };
	for (unsigned i = 0; i < 5; i++) {
		unsigned n = big_frag(f, order[i], 1480, 0x111,
				      order[i] == 2960);
		NS[0]->rx(f, n);
	}
	int r = NS[0]->recvfrom(s, rb, 70000, 0, NULL, NULL);
	CHECK(r == 4000 && !pat_check(3, 0, rb, 4000), "reassembled %d", r);
	CHECK(MIB("IpReasmDups") >= 1, "duplicates not counted");
	CHECK(NS[0]->recvfrom(s, rb, 70000, 0, NULL, NULL) == -EAGAIN,
	      "delivered twice");
	// a duplicate covering two held fragments exactly is tolerated too
	big_udp(3000, 6000, 4);
	NS[0]->rx(f, big_frag(f, 0, 1000, 0x112, 0));
	NS[0]->rx(f, big_frag(f, 1000, 1000, 0x112, 0));
	NS[0]->rx(f, big_frag(f, 0, 2000, 0x112, 0));
	NS[0]->rx(f, big_frag(f, 2000, 1008, 0x112, 1));
	r = NS[0]->recvfrom(s, rb, 70000, 0, NULL, NULL);
	CHECK(r == 3000 && !pat_check(4, 0, rb, 3000), "second: %d", r);
	free(rb);
	return 0;
}

static int t_frag_overlap(void)
{
	uint8_t f[1600], rb[5000];
	int s;
	unsigned q;

	setup_x();
	s = udp_sock(0, 6001);
	big_udp(3000, 6001, 5);
	NS[0]->rx(f, big_frag(f, 0, 1480, 0x222, 0));
	// overlaps the first one partially: the datagram is dead
	NS[0]->rx(f, big_frag(f, 1000, 1000, 0x222, 0));
	CHECK(MIB("IpReasmOverlaps") == 1, "overlap not counted");
	n0_kt_ipfrag(&q, NULL, NULL, NULL);
	CHECK(q == 0, "the queue survived the overlap (%u)", q);
	// the rest arrives: a new queue that never completes
	NS[0]->rx(f, big_frag(f, 1480, 1528, 0x222, 1));
	CHECK(NS[0]->recvfrom(s, rb, sizeof(rb), 0, NULL, NULL) == -EAGAIN,
	      "a datagram with overlapping fragments was delivered");
	// teardrop: the second fragment inside the first, shorter
	big_udp(2000, 6001, 6);
	NS[0]->rx(f, big_frag(f, 0, 1480, 0x223, 0));
	NS[0]->rx(f, big_frag(f, 8, 600, 0x223, 1));
	CHECK(NS[0]->recvfrom(s, rb, sizeof(rb), 0, NULL, NULL) == -EAGAIN,
	      "teardrop delivered something");
	// two different last fragments
	big_udp(2000, 6001, 7);
	NS[0]->rx(f, big_frag(f, 1480, 528, 0x224, 1));
	{
		// a "last" fragment ending elsewhere
		unsigned n = big_frag(f, 1480, 400, 0x224, 1);

		NS[0]->rx(f, n);
	}
	NS[0]->rx(f, big_frag(f, 0, 1480, 0x224, 0));
	CHECK(NS[0]->recvfrom(s, rb, sizeof(rb), 0, NULL, NULL) == -EAGAIN,
	      "inconsistent last fragments delivered something");
	return 0;
}

static int t_frag_timeout(void)
{
	uint8_t f[1600], r[1600];
	unsigned n, q;
	unsigned long long to;

	setup_x();
	udp_sock(0, 6002);
	// fragment 0 only: ICMP time exceeded (reassembly) after 30 s
	big_udp(3000, 6002, 8);
	NS[0]->rx(f, big_frag(f, 0, 1480, 0x333, 0));
	n0_kt_ipfrag(&q, NULL, NULL, NULL);
	CHECK(q == 1, "no queue");
	sim_run_for(29 * SEC);
	CHECK(!cap_next_proto(1, r, sizeof(r), 0), "an ICMP before 30 s");
	n = cap_next_proto(1, r, sizeof(r), 2 * SEC);
	CHECK(n, "no time exceeded after 30 s");
	CHECK(r[34] == 11 && r[35] == 1, "ICMP %u/%u", r[34], r[35]);
	CHECK(get32(r + 30) == HX, "to %08x", get32(r + 30));
	CHECK(get16(r + 42 + 4) == 0x333 && r[42 + 9] == 17,
	      "does not quote the first fragment");
	n0_kt_ipfrag(&q, NULL, NULL, &to);
	CHECK(q == 0 && to == 1, "queue %u timeouts %llu", q, to);
	CHECK(MIB("IpReasmTimeout") == 1, "not counted");
	// without fragment 0: silence
	NS[0]->rx(f, big_frag(f, 1480, 1528, 0x334, 1));
	sim_run_for(31 * SEC);
	CHECK(!cap_next_proto(1, r, sizeof(r), 0),
	      "time exceeded without fragment 0");
	n0_kt_ipfrag(&q, NULL, NULL, &to);
	CHECK(q == 0 && to == 2, "queue %u timeouts %llu", q, to);
	return 0;
}

static int t_frag_memory(void)
{
	uint8_t f[1600];
	unsigned q;
	unsigned long long mem, ev;

	setup_x();
	// 64 KB high, 48 KB low: a few dozen one-fragment queues
	n0_kt_ipfrag_thresh(64 * 1024, 48 * 1024);
	big_udp(3000, 6003, 9);
	for (unsigned i = 0; i < 200; i++)
		NS[0]->rx(f, big_frag(f, 0, 1480, (uint16_t)(0x4000 + i), 0));
	n0_kt_ipfrag(&q, &mem, &ev, NULL);
	CHECK(ev > 0, "nothing evicted (mem %llu, %u queues)", mem, q);
	CHECK(mem <= 64 * 1024 + 4096, "holding %llu bytes", mem);
	// the newest survive: complete the last one
	{
		int s = udp_sock(0, 6003);
		uint8_t rb[4000];

		NS[0]->rx(f, big_frag(f, 1480, 1528, 0x4000 + 199, 1));
		CHECK(NS[0]->recvfrom(s, rb, sizeof(rb), 0, NULL, NULL) == 3000,
		      "the youngest datagram was evicted");
	}
	// default thresholds: 4 MB / 3 MB, the pool of queues bounds too
	n0_kt_ipfrag_thresh(0, 0);
	for (unsigned i = 0; i < 3000; i++)
		NS[0]->rx(f, big_frag(f, 0, 1480, (uint16_t)(0x8000 + i), 0));
	n0_kt_ipfrag(&q, &mem, &ev, NULL);
	CHECK(mem <= 4u * 1024 * 1024 + 4096, "holding %llu bytes", mem);
	CHECK(q <= 1024, "%u queues", q);
	return 0;
}

static int t_frag_limits(void)
{
	uint8_t f[1600], rb[2000];
	int s;
	unsigned long long fails;

	setup_x();
	s = udp_sock(0, 6004);
	// a datagram that would exceed 65535 bytes
	big_udp(1000, 6004, 10);
	fails = MIB("IpReasmFails");
	NS[0]->rx(f, big_frag(f, 0, 1000, 0x555, 0));
	{
		uint8_t l4[600];
		unsigned n;

		memset(l4, 0x33, sizeof(l4));
		// offset 65000, 600 bytes: ends at 65600 > 65515
		n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, 600, 0x555,
			(uint16_t)(65000 / 8), 64, 0, NULL, 0);
		NS[0]->rx(f, n);
	}
	CHECK(MIB("IpReasmFails") > fails, "oversized datagram not refused");
	CHECK(NS[0]->recvfrom(s, rb, sizeof(rb), 0, NULL, NULL) == -EAGAIN,
	      "oversized delivered");
	// more than 1024 fragments in one queue
	fails = MIB("IpReasmFails");
	big_udp(20000, 6004, 11);
	for (unsigned off = 0; off + 16 <= 16 * 1100 && off < big_len; off += 16)
		NS[0]->rx(f, big_frag(f, off, 16, 0x556, 0));
	CHECK(MIB("IpReasmFails") > fails, "a 1100-fragment queue lived");
	// an empty non-last fragment kills the queue
	fails = MIB("IpReasmFails");
	big_udp(1000, 6004, 12);
	NS[0]->rx(f, big_frag(f, 0, 0, 0x557, 0));
	CHECK(MIB("IpReasmFails") > fails, "empty fragment accepted");
	return 0;
}

/* ================================================================== */
/* Output                                                              */
/* ================================================================== */

static unsigned cap_udp(uint8_t *r, unsigned cap)
{
	return cap_next_proto(17, r, cap, 100 * MS);
}

static int t_out_df_and_id(void)
{
	uint8_t r[1600], d[1400];
	int s = udp_sock(0, 0), c = udp_sock(0, 0);
	unsigned id1, id2;

	setup_x();
	memset(d, 1, sizeof(d));
	// unconnected, fits: DF, identification 0
	CHECK(NS[0]->sendto(s, d, 1000, 0, HX, 9) == 1000, "sendto");
	CHECK(cap_udp(r, sizeof(r)), "nothing sent");
	CHECK((get16(r + 20) & 0x4000) && get16(r + 18) == 0,
	      "frag %04x id %u", get16(r + 20), get16(r + 18));
	// connected: DF and its own counting identification
	CHECK(NS[0]->connect(c, HX, 9) == 0, "connect");
	NS[0]->send(c, d, 100, 0);
	CHECK(cap_udp(r, sizeof(r)), "nothing sent");
	id1 = get16(r + 18);
	NS[0]->send(c, d, 100, 0);
	CHECK(cap_udp(r, sizeof(r)), "nothing sent");
	id2 = get16(r + 18);
	CHECK((get16(r + 20) & 0x4000) && id2 == ((id1 + 1) & 0xffff),
	      "ids %u %u", id1, id2);
	// IP_MTU_DISCOVER DONT: no DF, a counting identification
	{
		int v = PMTU_DONT;

		CHECK(NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_MTU_DISCOVER, &v,
					sizeof(v)) == 0,
		      "IP_MTU_DISCOVER");
		NS[0]->sendto(s, d, 100, 0, HX, 9);
		CHECK(cap_udp(r, sizeof(r)), "nothing sent");
		id1 = get16(r + 18);
		NS[0]->sendto(s, d, 100, 0, HX, 9);
		CHECK(cap_udp(r, sizeof(r)), "nothing sent");
		id2 = get16(r + 18);
		CHECK(!(get16(r + 20) & 0x4000) && id2 == ((id1 + 1) & 0xffff),
		      "DONT: frag %04x ids %u %u", get16(r + 20), id1, id2);
		v = 7;
		CHECK(NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_MTU_DISCOVER, &v,
					sizeof(v)) == -EINVAL,
		      "mode 7 accepted");
	}
	// TTL, TOS
	{
		int ttl = 17, tos = 0x10, v = 0;
		unsigned l = sizeof(v);

		NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_TTL, &ttl, sizeof(ttl));
		NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_TOS, &tos, sizeof(tos));
		NS[0]->sendto(s, d, 10, 0, HX, 9);
		CHECK(cap_udp(r, sizeof(r)), "nothing sent");
		CHECK(r[22] == 17 && r[15] == 0x10, "ttl %u tos %02x", r[22],
		      r[15]);
		ttl = 0;
		CHECK(NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_TTL, &ttl,
					sizeof(ttl)) == -EINVAL,
		      "TTL 0 accepted");
		ttl = -1;
		NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_TTL, &ttl, sizeof(ttl));
		NS[0]->getsockopt(s, K_IPPROTO_IP, K_IP_TTL, &v, &l);
		CHECK(v == 64, "TTL -1 -> %d", v);
	}
	return 0;
}

static int t_out_fragments(void)
{
	uint8_t r[1600], *d = malloc(65536);
	int s = udp_sock(0, 7001);
	unsigned off = 0, id = 0, n, frags = 0;

	setup_x();
	pat_fill(1, 0, d, 65536);
	CHECK(NS[0]->sendto(s, d, 3000, 0, HX, 9) == 3000, "sendto");
	while ((n = cap_udp(r, sizeof(r))) != 0) {
		unsigned ff = get16(r + 20), len = get16(r + 16) - 20;

		CHECK(!(ff & 0x4000), "fragment with DF");
		CHECK((ff & 0x1fff) * 8 == off, "offset %u, expected %u",
		      (ff & 0x1fff) * 8, off);
		if (frags == 0)
			id = get16(r + 18);
		CHECK(get16(r + 18) == id, "identification changes");
		if (ff & 0x2000)
			CHECK(len % 8 == 0 && len <= 1480, "fragment of %u", len);
		off += len;
		frags++;
		if (!(ff & 0x2000))
			break;
	}
	CHECK(frags == 3 && off == 3008, "%u fragments, %u bytes", frags, off);
	CHECK(MIB("IpFragOKs") == 1 && MIB("IpFragCreates") == 3,
	      "counters %llu/%llu", MIB("IpFragOKs"), MIB("IpFragCreates"));
	// the largest datagram, and one byte more
	CHECK(NS[0]->sendto(s, d, 65507, 0, HX, 9) == 65507, "65507 bytes");
	CHECK(NS[0]->sendto(s, d, 65508, 0, HX, 9) == -EMSGSIZE,
	      "65508 bytes accepted");
	free(d);
	return 0;
}

static int t_pmtu_do(void)
{
	uint8_t r[1600], d[3000] = { 0 }, eb[100];
	int s = udp_sock(0, 7002), v = PMTU_DO, on = 1;
	struct kt_msg m;

	setup_x();
	NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_MTU_DISCOVER, &v, sizeof(v));
	CHECK(NS[0]->sendto(s, d, 2000, 0, HX, 9) == -EMSGSIZE,
	      "DO: 2000 bytes not refused");
	CHECK(NS[0]->sendto(s, d, 1472, 0, HX, 9) == 1472, "DO: 1472 refused");
	CHECK(cap_udp(r, sizeof(r)) && (get16(r + 20) & 0x4000), "DO: no DF");
	// with IP_RECVERR the refusal is also on the error queue
	NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_RECVERR, &on, sizeof(on));
	CHECK(NS[0]->sendto(s, d, 2000, 0, HX, 9) == -EMSGSIZE, "DO again");
	CHECK(n0_kt_recvmsg(s, eb, sizeof(eb), K_MSG_ERRQUEUE, &m) == 0,
	      "error queue read %d", m.ret);
	CHECK(m.have_err && m.ee_errno == EMSGSIZE && m.ee_origin == 1 &&
		      m.ee_info == 1500,
	      "ee errno %u origin %u info %u", m.ee_errno, m.ee_origin,
	      m.ee_info);
	CHECK(m.flags & K_MSG_ERRQUEUE, "MSG_ERRQUEUE not in msg_flags");
	CHECK(n0_kt_recvmsg(s, eb, sizeof(eb), K_MSG_ERRQUEUE, &m) == -EAGAIN,
	      "error queue not empty");
	return 0;
}

/* An ICMP fragmentation-needed from `router' (HY) about our datagram to
 * `dst', announcing `mtu'. */
/* "Fragmentation needed" from the router HY about a datagram of protocol
 * `proto' we sent to `dst' (ports, or ICMP type/code, as given). */
static void frag_needed_proto(uint8_t proto, uint32_t dst, unsigned mtu,
			      unsigned sport, unsigned dport)
{
	uint8_t q[28], l4[100], f[200];
	unsigned n;

	memset(q, 0, sizeof(q));
	q[0] = 0x45;
	put16(q + 2, 1400);
	q[6] = 0x40;
	q[8] = 64;
	q[9] = proto;
	put32(q + 12, ME);
	put32(q + 16, dst);
	put16(q + 10, inet_csum(q, 20, 0));
	put16(q + 20, sport);
	put16(q + 22, dport);
	put16(q + 24, 1380);
	n = icmpm(l4, 3, 4, mtu & 0xffff, q, sizeof(q));
	n = ipf(f, hy_mac, NULL, HY, ME, 1, l4, n, 77, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
}

static void frag_needed(uint32_t dst, unsigned mtu, unsigned sport,
			unsigned dport)
{
	frag_needed_proto(17, dst, mtu, sport, dport);
}

/* A report changes the path MTU only when it is about a datagram one of
 * our sockets may have sent, and that socket takes path-MTU reports. */
static int t_pmtu_report_rules(void)
{
	int u = udp_sock(0, 7101), rs = n0_kt_raw_socket(1), v;

	setup_x();
	CHECK(NS[0]->route_add(0xC0000200u, 0xFFFFFF00u, HY) == 0, "route");
	// nobody here sent it
	frag_needed(0xC0000201u, 1000, 7100, 9);
	CHECK(n0_kt_pmtu(0xC0000201u) == 0, "a report about no socket's "
	      "datagram set %u", n0_kt_pmtu(0xC0000201u));
	// the UDP socket that sent it ignores the path MTU
	v = PMTU_OMIT;
	NS[0]->setsockopt(u, K_IPPROTO_IP, K_IP_MTU_DISCOVER, &v, sizeof(v));
	frag_needed(0xC0000202u, 1000, 7101, 9);
	CHECK(n0_kt_pmtu(0xC0000202u) == 0, "OMIT socket: %u",
	      n0_kt_pmtu(0xC0000202u));
	v = PMTU_INTERFACE;
	NS[0]->setsockopt(u, K_IPPROTO_IP, K_IP_MTU_DISCOVER, &v, sizeof(v));
	frag_needed(0xC0000202u, 1000, 7101, 9);
	CHECK(n0_kt_pmtu(0xC0000202u) == 0, "INTERFACE socket: %u",
	      n0_kt_pmtu(0xC0000202u));
	v = PMTU_WANT;
	NS[0]->setsockopt(u, K_IPPROTO_IP, K_IP_MTU_DISCOVER, &v, sizeof(v));
	frag_needed(0xC0000202u, 1000, 7101, 9);
	CHECK(n0_kt_pmtu(0xC0000202u) == 1000, "WANT socket: %u",
	      n0_kt_pmtu(0xC0000202u));
	// raw: a socket connected to another destination did not send it
	CHECK(rs >= 0 && NS[0]->connect(rs, 0xC0000208u, 0) == 0, "raw");
	frag_needed_proto(1, 0xC0000209u, 1000, 0x0800, 0);
	CHECK(n0_kt_pmtu(0xC0000209u) == 0, "raw socket connected elsewhere "
	      "matched: %u", n0_kt_pmtu(0xC0000209u));
	frag_needed_proto(1, 0xC0000208u, 1000, 0x0800, 0);
	CHECK(n0_kt_pmtu(0xC0000208u) == 1000, "raw socket's destination: %u",
	      n0_kt_pmtu(0xC0000208u));
	v = PMTU_OMIT;
	NS[0]->setsockopt(rs, K_IPPROTO_IP, K_IP_MTU_DISCOVER, &v, sizeof(v));
	frag_needed_proto(1, 0xC0000208u, 800, 0x0800, 0);
	CHECK(n0_kt_pmtu(0xC0000208u) == 1000, "raw OMIT socket lowered it "
	      "to %u", n0_kt_pmtu(0xC0000208u));
	return 0;
}

static int t_pmtu_frag_needed(void)
{
	uint8_t r[1600], d[2000] = { 0 };
	int s = udp_sock(0, 7003), sdo = udp_sock(0, 7004), v = PMTU_DO;
	unsigned n, l;
	int mtu = 0;

	setup_x();
	// a route through HY to 192.0.2.0/24 (the path whose MTU shrinks)
	CHECK(NS[0]->route_add(0xC0000200u, 0xFFFFFF00u, HY) == 0, "route");
	NS[0]->setsockopt(sdo, K_IPPROTO_IP, K_IP_MTU_DISCOVER, &v, sizeof(v));
	frag_needed(0xC0000201u, 1000, 7003, 9);
	CHECK(n0_kt_pmtu(0xC0000201u) == 1000, "pmtu %u", n0_kt_pmtu(0xC0000201u));
	CHECK(MIB("IcmpPmtuUpdates") == 1, "update not counted");
	// WANT: 1200 bytes now fragmented by us at 1000
	CHECK(NS[0]->sendto(s, d, 1200, 0, 0xC0000201u, 9) == 1200, "sendto");
	n = cap_udp(r, sizeof(r));
	CHECK(n && get16(r + 16) <= 1000 && (get16(r + 20) & 0x2000),
	      "first fragment %u bytes, frag %04x", n ? get16(r + 16) : 0,
	      get16(r + 20));
	// DO: refused, IP_MTU says 1000
	CHECK(NS[0]->sendto(sdo, d, 1200, 0, 0xC0000201u, 9) == -EMSGSIZE,
	      "DO after frag-needed");
	NS[0]->connect(sdo, 0xC0000201u, 9);
	l = sizeof(mtu);
	CHECK(NS[0]->getsockopt(sdo, K_IPPROTO_IP, K_IP_MTU, &mtu, &l) == 0 &&
		      mtu == 1000,
	      "IP_MTU %d", mtu);
	// never raised by a report
	frag_needed(0xC0000201u, 1400, 7003, 9);
	CHECK(n0_kt_pmtu(0xC0000201u) == 1000, "raised to %u",
	      n0_kt_pmtu(0xC0000201u));
	// below the floor: 552, and DF goes away for WANT
	frag_needed(0xC0000201u, 300, 7003, 9);
	CHECK(n0_kt_pmtu(0xC0000201u) == 552, "floor %u",
	      n0_kt_pmtu(0xC0000201u));
	cap_start(0, 1);
	NS[0]->sendto(s, d, 400, 0, 0xC0000201u, 9);
	n = cap_udp(r, sizeof(r));
	CHECK(n && !(get16(r + 20) & 0x4000), "DF at the locked floor");
	// it expires after 10 minutes
	sim_run_for(601 * SEC);
	CHECK(n0_kt_pmtu(0xC0000201u) == 0, "still %u after 601 s",
	      n0_kt_pmtu(0xC0000201u));
	return 0;
}

/* ================================================================== */
/* ICMP                                                                */
/* ================================================================== */

static int t_icmp_broadcast_echo(void)
{
	uint8_t f[1600], r[1600];
	unsigned n;

	setup_x();
	n = echo_from(f, HX, SUBNET_BCAST, bcast, 1, NULL, 0);
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)),
	      "answered an echo to the subnet broadcast");
	n = echo_from(f, HX, 0xFFFFFFFFu, bcast, 2, NULL, 0);
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)),
	      "answered an echo to the limited broadcast");
	n = echo_from(f, HX, 0xE0000001u, NULL, 3, NULL, 0);
	f[0] = 0x01;
	f[1] = 0x00;
	f[2] = 0x5e;
	f[3] = 0;
	f[4] = 0;
	f[5] = 1;
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)),
	      "answered an echo to 224.0.0.1");
	// unicast echo replies are never rate-limited
	for (int i = 0; i < 100; i++) {
		n = echo_from(f, HX, ME, NULL, (uint16_t)(10 + i), NULL, 0);
		CHECK(rx_and_reply(f, n, 1, r, sizeof(r)), "echo %d", i);
	}
	return 0;
}

static int t_icmp_timestamp(void)
{
	uint8_t f[1600], r[1600], l4[40], ts[12] = { 0, 1, 2, 3 };
	unsigned n;

	setup_x();
	n = icmpm(l4, 13, 0, 0x12340001u, ts, 12);
	n = ipf(f, hx_mac, NULL, HX, ME, 1, l4, n, 9, 0, 64, 0, NULL, 0);
	n = rx_and_reply(f, n, 1, r, sizeof(r));
	CHECK(n == 14 + 20 + 20, "timestamp reply %u bytes", n);
	CHECK(r[34] == 14 && get32(r + 38) == 0x12340001u, "type %u", r[34]);
	CHECK(!memcmp(r + 42, ts, 4), "originate not echoed");
	CHECK(get32(r + 46) < 86400000u && get32(r + 46) == get32(r + 50),
	      "receive/transmit %u %u", get32(r + 46), get32(r + 50));
	CHECK(!frame_csum_bad(r, n), "checksum");
	// too short: ignored
	n = icmpm(l4, 13, 0, 1, ts, 4);
	n = ipf(f, hx_mac, NULL, HX, ME, 1, l4, n, 10, 0, 64, 0, NULL, 0);
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered a short request");
	return 0;
}

static int t_icmp_error_rules(void)
{
	uint8_t f[1600], r[1600], l4[200], d[20] = { 0 };
	unsigned n;

	setup_x();
	// UDP to a closed port: port unreachable, quoting the datagram
	n = udpd(l4, HX, ME, 1234, 9999, d, 20, 0);
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, 5, 0, 64, 0x0C, NULL, 0);
	n = rx_and_reply(f, n, 1, r, sizeof(r));
	CHECK(n, "no port unreachable");
	CHECK(r[34] == 3 && r[35] == 3, "ICMP %u/%u", r[34], r[35]);
	CHECK(r[15] == 0xCC, "error TOS %02x", r[15]);
	CHECK(!(get16(r + 20) & 0x4000), "an ICMP error with DF");
	CHECK(get32(r + 26) == ME, "from %08x", get32(r + 26));
	CHECK(n == 14 + 20 + 8 + 20 + 28, "quotes %u bytes", n - 42);
	CHECK(!frame_csum_bad(r, n), "checksum");
	// ... but not for a datagram to the broadcast address
	n = udpd(l4, HX, SUBNET_BCAST, 1234, 9999, d, 20, 0);
	n = ipf(f, hx_mac, bcast, HX, SUBNET_BCAST, 17, l4, n, 6, 0, 64, 0,
		NULL, 0);
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "error about a broadcast");
	CHECK(MIB("UdpIgnoredMulti") == 1, "IgnoredMulti %llu",
	      MIB("UdpIgnoredMulti"));
	// ... nor for a non-first fragment
	{
		n = udpd(l4, HX, ME, 1234, 9999, d, 20, 0);
		n = ipf(f, hx_mac, NULL, HX, ME, 99, l4, n, 7, 185, 64, 0,
			NULL, 0);
		NS[0]->rx(f, n); // a lone trailing fragment: no queue answer
		CHECK(!cap_next_proto(1, r, sizeof(r), 100 * MS),
		      "error about a fragment");
	}
	// ... nor about an ICMP error
	{
		uint8_t q[28];

		memset(q, 0, sizeof(q));
		q[0] = 0x45;
		put16(q + 2, 28);
		q[9] = 17;
		put32(q + 12, ME);
		put32(q + 16, HX);
		put16(q + 10, inet_csum(q, 20, 0));
		n = icmpm(l4, 3, 3, 0, q, sizeof(q));
		// an error about a datagram of ours: absorbed, never answered
		n = ipf(f, hx_mac, NULL, HX, ME, 1, l4, n, 8, 0, 64, 0, NULL,
			0);
		NS[0]->rx(f, n);
		CHECK(!cap_next_proto(1, r, sizeof(r), 100 * MS),
		      "answered an ICMP error");
	}
	// redirects and source quench change nothing and draw no answer
	{
		uint8_t q[28] = { 0x45 };

		n = icmpm(l4, 5, 1, HY, q, sizeof(q));
		n = ipf(f, hx_mac, NULL, HX, ME, 1, l4, n, 9, 0, 64, 0, NULL,
			0);
		CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered redirect");
		n = icmpm(l4, 4, 0, 0, q, sizeof(q));
		n = ipf(f, hx_mac, NULL, HX, ME, 1, l4, n, 10, 0, 64, 0, NULL,
			0);
		CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered quench");
		CHECK(MIB("IcmpInRedirects") == 1 && MIB("IcmpInSrcQuenchs") == 1,
		      "redirect/quench not counted");
	}
	// a corrupt ICMP message is counted and dropped
	n = echo_from(f, HX, ME, NULL, 1, NULL, 0);
	f[14 + 20 + 9] ^= 1;
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered a bad checksum");
	CHECK(MIB("IcmpInCsumErrors") == 1, "not counted");
	return 0;
}

static int t_icmp_rate_limits(void)
{
	uint8_t f[1600], r[1600], l4[200], d[20] = { 0 };
	unsigned n;
	int got = 0;

	setup_x();
	// one host: a burst of six, then one a second
	for (int i = 0; i < 20; i++) {
		n = udpd(l4, HX, ME, 1234, 9000 + i, d, 20, 0);
		n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, (uint16_t)i, 0, 64,
			0, NULL, 0);
		NS[0]->rx(f, n);
	}
	while (cap_next_proto(1, r, sizeof(r), 10 * MS))
		got++;
	CHECK(got == 6, "%d port unreachables in a burst", got);
	sim_run_for(3 * SEC);
	got = 0;
	for (int i = 0; i < 20; i++) {
		n = udpd(l4, HX, ME, 1234, 9000 + i, d, 20, 0);
		n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, (uint16_t)i, 0, 64,
			0, NULL, 0);
		NS[0]->rx(f, n);
	}
	while (cap_next_proto(1, r, sizeof(r), 10 * MS))
		got++;
	CHECK(got == 3, "%d after three seconds", got);
	// a second host has its own budget
	got = 0;
	for (int i = 0; i < 4; i++) {
		n = udpd(l4, HY, ME, 1234, 9000 + i, d, 20, 0);
		n = ipf(f, hy_mac, NULL, HY, ME, 17, l4, n, (uint16_t)i, 0, 64,
			0, NULL, 0);
		NS[0]->rx(f, n);
	}
	while (cap_next_proto(1, r, sizeof(r), 10 * MS))
		got++;
	CHECK(got == 4, "%d for the second host", got);
	// the global bucket: 50 at once across hosts
	n0_kt_icmp_rl_reset();
	got = 0;
	for (int h = 0; h < 80; h++) {
		uint32_t src = (10u << 24) | (uint32_t)(100 + h);
		uint8_t mac[6] = { 2, 0, 0, 0, 1, (uint8_t)h };

		NS[0]->arp_add(src, mac);
		n = udpd(l4, src, ME, 1234, 9000, d, 20, 0);
		n = ipf(f, mac, NULL, src, ME, 17, l4, n, 1, 0, 64, 0, NULL, 0);
		NS[0]->rx(f, n);
	}
	while (cap_next_proto(1, r, sizeof(r), 10 * MS))
		got++;
	CHECK(got >= 50 && got <= 52, "%d errors from the global bucket", got);
	CHECK(MIB("IcmpOutRateLimitGlobal") >= 28, "global limit counted %llu",
	      MIB("IcmpOutRateLimitGlobal"));
	return 0;
}

/* ================================================================== */
/* UDP                                                                 */
/* ================================================================== */

static int inject_udp(uint32_t sip, const uint8_t *smac, uint32_t dip,
		      const uint8_t *dmac, unsigned sport, unsigned dport,
		      unsigned seed, unsigned len)
{
	uint8_t f[1600], l4[1500], d[1400];

	pat_fill(seed, 0, d, len);
	unsigned n = udpd(l4, sip, dip, sport, dport, d, len, 0);
	n = ipf(f, smac, dmac, sip, dip, 17, l4, n, (uint16_t)seed, 0, 64, 0,
		NULL, 0);
	NS[0]->rx(f, n);
	return 0;
}

static int got_seed(int s, unsigned seed, unsigned len)
{
	uint8_t b[1500];
	int r = NS[0]->recvfrom(s, b, sizeof(b), 0, NULL, NULL);

	return r == (int)len && !pat_check(seed, 0, b, len);
}

static int t_udp_lookup(void)
{
	int one = 1;
	int any, bound, conn, r;

	setup_x();
	any = NS[0]->socket(K_SOCK_DGRAM);
	bound = NS[0]->socket(K_SOCK_DGRAM);
	conn = NS[0]->socket(K_SOCK_DGRAM);
	for (int s = any; s <= conn; s++) {
		NS[0]->set_nonblock(s, 1);
		NS[0]->setsockopt(s, K_SOL_SOCKET, K_SO_REUSEADDR, &one,
				  sizeof(one));
	}
	CHECK(NS[0]->bind(any, 0, 5000) == 0, "bind any");
	CHECK(NS[0]->bind(bound, ME, 5000) == 0, "bind addr");
	CHECK(NS[0]->bind(conn, 0, 5000) == 0, "bind conn");
	CHECK(NS[0]->connect(conn, HX, 6000) == 0, "connect");
	// from the connected peer: the connected socket
	inject_udp(HX, hx_mac, ME, NULL, 6000, 5000, 1, 100);
	CHECK(got_seed(conn, 1, 100), "connected socket did not get its peer's");
	// from elsewhere: the one bound to the address
	inject_udp(HX, hx_mac, ME, NULL, 6001, 5000, 2, 100);
	CHECK(got_seed(bound, 2, 100), "bound socket did not win");
	inject_udp(HY, hy_mac, ME, NULL, 6000, 5000, 3, 100);
	CHECK(got_seed(bound, 3, 100), "bound socket did not win (HY)");
	// without it: the wildcard one
	NS[0]->close(bound);
	inject_udp(HY, hy_mac, ME, NULL, 6000, 5000, 4, 100);
	CHECK(got_seed(any, 4, 100), "wildcard socket did not get it");
	// a connected socket never sees other sources
	NS[0]->close(any);
	inject_udp(HY, hy_mac, ME, NULL, 6000, 5000, 5, 100);
	r = NS[0]->recvfrom(conn, (uint8_t[200]){ 0 }, 200, 0, NULL, NULL);
	CHECK(r == -EAGAIN, "connected socket got a stranger's datagram (%d)",
	      r);
	CHECK(MIB("UdpNoPorts") == 1, "NoPorts %llu", MIB("UdpNoPorts"));
	// disconnect (AF_UNSPEC): everybody's again
	CHECK(n0_kt_disconnect(conn) == 0, "disconnect");
	inject_udp(HY, hy_mac, ME, NULL, 6000, 5000, 6, 100);
	CHECK(got_seed(conn, 6, 100), "after disconnect");
	return 0;
}

static int t_udp_bind_rules(void)
{
	int one = 1;
	int a = NS[0]->socket(K_SOCK_DGRAM), b = NS[0]->socket(K_SOCK_DGRAM);
	int c = NS[0]->socket(K_SOCK_DGRAM), d = NS[0]->socket(K_SOCK_DGRAM);
	unsigned ip, port;

	CHECK(NS[0]->bind(a, 0, 5100) == 0, "bind");
	CHECK(NS[0]->bind(b, 0, 5100) == -EADDRINUSE, "second bind allowed");
	CHECK(NS[0]->bind(b, 0x7F000001u, 5100) == -EADDRINUSE,
	      "wildcard vs 127.0.0.1 allowed");
	CHECK(NS[0]->bind(c, ME, 5101) == 0, "bind ME:5101");
	CHECK(NS[0]->bind(b, 0x7F000001u, 5101) == 0,
	      "different addresses clash");
	CHECK(NS[0]->bind(d, (10u << 24) | 77, 5102) == -EADDRNOTAVAIL,
	      "bound a foreign address");
	CHECK(NS[0]->bind(c, 0, 5200) == -EINVAL, "rebind allowed");
	// SO_REUSEADDR needs both
	d = NS[0]->socket(K_SOCK_DGRAM);
	NS[0]->setsockopt(d, K_SOL_SOCKET, K_SO_REUSEADDR, &one, sizeof(one));
	CHECK(NS[0]->bind(d, 0, 5100) == -EADDRINUSE, "one-sided REUSEADDR");
	// SO_REUSEPORT: both
	{
		int e = NS[0]->socket(K_SOCK_DGRAM),
		    g = NS[0]->socket(K_SOCK_DGRAM);

		NS[0]->setsockopt(e, K_SOL_SOCKET, K_SO_REUSEPORT, &one,
				  sizeof(one));
		NS[0]->setsockopt(g, K_SOL_SOCKET, K_SO_REUSEPORT, &one,
				  sizeof(one));
		CHECK(NS[0]->bind(e, 0, 5300) == 0 &&
			      NS[0]->bind(g, 0, 5300) == 0,
		      "REUSEPORT group");
		CHECK(NS[0]->bind(d, 0, 5300) == -EADDRINUSE,
		      "joined a REUSEPORT group without the option");
	}
	// port 0: an ephemeral port, reported by getsockname
	{
		int e = NS[0]->socket(K_SOCK_DGRAM);

		CHECK(NS[0]->bind(e, 0, 0) == 0, "ephemeral");
		NS[0]->sockname(e, 0, &ip, &port);
		CHECK(port >= 49152 && ip == 0, "got %08x:%u", ip, port);
	}
	// autobind on send: the wildcard address
	{
		int e = NS[0]->socket(K_SOCK_DGRAM);

		NS[0]->arp_add(HX, hx_mac);
		NS[0]->sendto(e, "x", 1, 0, HX, 9);
		NS[0]->sockname(e, 0, &ip, &port);
		CHECK(port >= 49152 && ip == 0, "autobound %08x:%u", ip, port);
		// connect fills in the address
		NS[0]->connect(e, HX, 9);
		NS[0]->sockname(e, 0, &ip, &port);
		CHECK(ip == ME, "connected local address %08x", ip);
	}
	return 0;
}

static int t_udp_reuseport(void)
{
	int one = 1, s[4], hits[4] = { 0 }, first[64];

	setup_x();
	for (int i = 0; i < 4; i++) {
		s[i] = NS[0]->socket(K_SOCK_DGRAM);
		NS[0]->set_nonblock(s[i], 1);
		NS[0]->setsockopt(s[i], K_SOL_SOCKET, K_SO_REUSEPORT, &one,
				  sizeof(one));
		CHECK(NS[0]->bind(s[i], 0, 7000) == 0, "bind %d", i);
	}
	for (int round = 0; round < 2; round++) {
		for (int fl = 0; fl < 64; fl++) {
			int who = -1;

			inject_udp(HX, hx_mac, ME, NULL, 20000 + fl, 7000,
				   (unsigned)fl, 50);
			for (int i = 0; i < 4; i++)
				if (got_seed(s[i], (unsigned)fl, 50))
					who = i;
			CHECK(who >= 0, "flow %d delivered nowhere", fl);
			if (round == 0) {
				first[fl] = who;
				hits[who]++;
			} else {
				CHECK(first[fl] == who,
				      "flow %d moved from %d to %d", fl,
				      first[fl], who);
			}
		}
	}
	for (int i = 0; i < 4; i++)
		CHECK(hits[i] >= 6, "member %d got %d of 64 flows", i, hits[i]);
	return 0;
}

static int t_udp_broadcast_fanout(void)
{
	int one = 1, s[3], spec;
	uint8_t b[200];

	setup_x();
	for (int i = 0; i < 3; i++) {
		s[i] = NS[0]->socket(K_SOCK_DGRAM);
		NS[0]->set_nonblock(s[i], 1);
		NS[0]->setsockopt(s[i], K_SOL_SOCKET, K_SO_REUSEADDR, &one,
				  sizeof(one));
		CHECK(NS[0]->bind(s[i], 0, 8000) == 0, "bind %d", i);
	}
	spec = NS[0]->socket(K_SOCK_DGRAM);
	NS[0]->set_nonblock(spec, 1);
	NS[0]->setsockopt(spec, K_SOL_SOCKET, K_SO_REUSEADDR, &one, sizeof(one));
	CHECK(NS[0]->bind(spec, ME, 8000) == 0, "bind specific");
	inject_udp(HX, hx_mac, SUBNET_BCAST, bcast, 1234, 8000, 7, 100);
	for (int i = 0; i < 3; i++)
		CHECK(got_seed(s[i], 7, 100), "socket %d missed the broadcast", i);
	CHECK(NS[0]->recvfrom(spec, b, sizeof(b), 0, NULL, NULL) == -EAGAIN,
	      "a socket bound to the unicast address got a broadcast");
	inject_udp(HX, hx_mac, 0xFFFFFFFFu, bcast, 1234, 8000, 8, 100);
	for (int i = 0; i < 3; i++)
		CHECK(got_seed(s[i], 8, 100), "socket %d missed 255.255.255.255",
		      i);
	// multicast: only after the group is joined here
	inject_udp(HX, hx_mac, 0xEF010203u, NULL, 1234, 8000, 9, 100);
	CHECK(NS[0]->recvfrom(s[0], b, sizeof(b), 0, NULL, NULL) == -EAGAIN,
	      "a group nobody joined was delivered");
	CHECK(n0_kt_mc_join(s[1], 0xEF010203u, 0, 0) == 0, "join");
	CHECK(n0_kt_mc_join(s[1], 0xEF010203u, 0, 0) == -EADDRINUSE,
	      "joined twice");
	inject_udp(HX, hx_mac, 0xEF010203u, NULL, 1234, 8000, 10, 100);
	for (int i = 0; i < 3; i++)
		CHECK(got_seed(s[i], 10, 100), "socket %d missed the group", i);
	CHECK(n0_kt_mc_join(s[1], 0xEF010203u, 0, 1) == 0, "leave");
	// sending a broadcast needs SO_BROADCAST
	CHECK(NS[0]->sendto(s[0], b, 10, 0, SUBNET_BCAST, 8000) == -EACCES,
	      "broadcast without SO_BROADCAST");
	CHECK(NS[0]->sendto(s[0], b, 10, 0, 0xFFFFFFFFu, 8000) == -EACCES,
	      "limited broadcast without SO_BROADCAST");
	NS[0]->setsockopt(s[0], K_SOL_SOCKET, K_SO_BROADCAST, &one, sizeof(one));
	pat_fill(11, 0, b, 30);
	CHECK(NS[0]->sendto(s[0], b, 30, 0, SUBNET_BCAST, 8000) == 30,
	      "broadcast with SO_BROADCAST");
	// it goes out, and comes back to every local listener of the port
	(void)cap_udp(b + 100, 100);
	for (int i = 0; i < 3; i++)
		CHECK(got_seed(s[i], 11, 30), "socket %d missed our broadcast",
		      i);
	return 0;
}

static int t_udp_multicast_out(void)
{
	int one = 1, zero = 0, rx, tx;
	uint8_t r[1600], b[100];
	unsigned n;

	setup_x();
	rx = udp_sock(0, 0);
	NS[0]->setsockopt(rx, K_SOL_SOCKET, K_SO_REUSEADDR, &one, sizeof(one));
	CHECK(NS[0]->bind(rx, 0, 8100) == 0, "bind");
	CHECK(n0_kt_mc_join(rx, 0xEF000001u, 0, 0) == 0, "join");
	tx = udp_sock(0, 0);
	pat_fill(1, 0, b, 40);
	CHECK(NS[0]->sendto(tx, b, 40, 0, 0xEF000001u, 8100) == 40, "send");
	n = cap_udp(r, sizeof(r));
	CHECK(n && r[22] == 1, "multicast TTL %u", n ? r[22] : 0);
	CHECK(r[0] == 0x01 && r[1] == 0 && r[2] == 0x5e && r[5] == 1,
	      "group MAC");
	CHECK(got_seed(rx, 1, 40), "not looped back to the member");
	// IP_MULTICAST_TTL / LOOP
	{
		int ttl = 5;

		NS[0]->setsockopt(tx, K_IPPROTO_IP, K_IP_MULTICAST_TTL, &ttl,
				  sizeof(ttl));
		NS[0]->setsockopt(tx, K_IPPROTO_IP, K_IP_MULTICAST_LOOP, &zero,
				  sizeof(zero));
		pat_fill(2, 0, b, 40);
		NS[0]->sendto(tx, b, 40, 0, 0xEF000001u, 8100);
		n = cap_udp(r, sizeof(r));
		CHECK(n && r[22] == 5, "IP_MULTICAST_TTL not applied");
		CHECK(NS[0]->recvfrom(rx, r, sizeof(r), 0, NULL, NULL) ==
			      -EAGAIN,
		      "looped back with IP_MULTICAST_LOOP 0");
	}
	return 0;
}

static int t_udp_rcvbuf(void)
{
	int s = udp_sock(0, 9100), v = 4096, got = 0;
	unsigned l = sizeof(v);
	uint8_t b[1500];

	setup_x();
	// the default holds a lot
	for (int i = 0; i < 60; i++)
		inject_udp(HX, hx_mac, ME, NULL, 1234, 9100, (unsigned)i, 1000);
	while (NS[0]->recvfrom(s, b, sizeof(b), 0, NULL, NULL) == 1000)
		got++;
	CHECK(got == 60, "default buffer took %d of 60", got);
	NS[0]->getsockopt(s, K_SOL_SOCKET, K_SO_RCVBUF, &v, &l);
	CHECK(v == 212992, "default SO_RCVBUF %d", v);
	// a small one: doubled, and full quickly
	v = 4096;
	NS[0]->setsockopt(s, K_SOL_SOCKET, K_SO_RCVBUF, &v, sizeof(v));
	NS[0]->getsockopt(s, K_SOL_SOCKET, K_SO_RCVBUF, &v, &l);
	CHECK(v == 8192, "SO_RCVBUF 4096 reads back %d", v);
	got = 0;
	unsigned long long e0 = MIB("UdpRcvbufErrors");
	for (int i = 0; i < 20; i++)
		inject_udp(HX, hx_mac, ME, NULL, 1234, 9100, (unsigned)i, 1000);
	while (NS[0]->recvfrom(s, b, sizeof(b), 0, NULL, NULL) == 1000)
		got++;
	CHECK(got >= 1 && got <= 8, "8 KB buffer took %d datagrams", got);
	CHECK(MIB("UdpRcvbufErrors") - e0 == (unsigned long long)(20 - got),
	      "drops counted %llu, %d lost", MIB("UdpRcvbufErrors") - e0,
	      20 - got);
	return 0;
}

static int t_udp_recv_flags(void)
{
	int s = udp_sock(0, 9200);
	uint8_t b[2000];

	setup_x();
	CHECK(NS[0]->recvfrom(s, b, sizeof(b), K_MSG_DONTWAIT, NULL, NULL) ==
		      -EAGAIN,
	      "DONTWAIT on an empty socket");
	inject_udp(HX, hx_mac, ME, NULL, 1234, 9200, 1, 500);
	CHECK(NS[0]->recvfrom(s, b, 100, K_MSG_PEEK | K_MSG_TRUNC, NULL,
			      NULL) == 500,
	      "PEEK|TRUNC length");
	CHECK(NS[0]->recvfrom(s, b, 100, K_MSG_PEEK, NULL, NULL) == 100,
	      "PEEK short");
	CHECK(!pat_check(1, 0, b, 100), "peeked bytes");
	{
		int avail = 0;

		NS[0]->ioctl(s, K_FIONREAD, &avail);
		CHECK(avail == 500, "FIONREAD %d", avail);
	}
	{
		struct kt_msg m;

		CHECK(n0_kt_recvmsg(s, b, 200, 0, &m) == 200, "recvmsg");
		CHECK(m.flags & K_MSG_TRUNC, "no MSG_TRUNC in msg_flags");
		CHECK(m.src_ip == HX && m.src_port == 1234, "from %08x:%u",
		      m.src_ip, m.src_port);
	}
	CHECK(NS[0]->recvfrom(s, b, sizeof(b), 0, NULL, NULL) == -EAGAIN,
	      "the truncated datagram is still there");
	return 0;
}

static int t_udp_cmsgs(void)
{
	int s = udp_sock(0, 9300), on = 1;
	uint8_t f[1600], l4[200], b[200], d[50];
	struct kt_msg m;
	unsigned n;

	setup_x();
	NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_PKTINFO, &on, sizeof(on));
	NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_RECVTTL, &on, sizeof(on));
	NS[0]->setsockopt(s, K_IPPROTO_IP, K_IP_RECVTOS, &on, sizeof(on));
	pat_fill(3, 0, d, sizeof(d));
	n = udpd(l4, HX, ME, 1234, 9300, d, 50, 0);
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, 1, 0, 33, 0x28, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(n0_kt_recvmsg(s, b, sizeof(b), 0, &m) == 50, "recvmsg %d", m.ret);
	CHECK(m.have_pktinfo && m.pkt_addr == ME && m.pkt_ifindex >= 2,
	      "pktinfo %d addr %08x ifindex %d", m.have_pktinfo, m.pkt_addr,
	      m.pkt_ifindex);
	CHECK(m.have_ttl && m.ttl == 33, "ttl %d", m.ttl);
	CHECK(m.have_tos && m.tos == 0x28, "tos %d", m.tos);
	return 0;
}

static int t_udp_checksums(void)
{
	int s = udp_sock(0, 9400);
	uint8_t f[1600], l4[200], d[50], b[200];
	unsigned n;

	setup_x();
	pat_fill(1, 0, d, sizeof(d));
	// checksum 0: "not computed", accepted
	n = udpd(l4, HX, ME, 1234, 9400, d, 50, 1);
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, 1, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(s, b, sizeof(b), 0, NULL, NULL) == 50,
	      "zero checksum refused");
	// a wrong one: dropped and counted
	n = udpd(l4, HX, ME, 1234, 9400, d, 50, 2);
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, 2, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(s, b, sizeof(b), 0, NULL, NULL) == -EAGAIN,
	      "bad checksum accepted");
	CHECK(MIB("UdpInCsumErrors") == 1 && MIB("UdpInErrors") == 1,
	      "counters %llu %llu", MIB("UdpInCsumErrors"), MIB("UdpInErrors"));
	// a length field past the datagram, and below the header
	n = udpd(l4, HX, ME, 1234, 9400, d, 50, 0);
	put16(l4 + 4, 200);
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, 3, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	n = udpd(l4, HX, ME, 1234, 9400, d, 50, 0);
	put16(l4 + 4, 4);
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, 4, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(MIB("UdpInErrors") == 3, "bad lengths %llu", MIB("UdpInErrors"));
	// a shorter UDP length than the IP payload: trimmed to it
	n = udpd(l4, HX, ME, 1234, 9400, d, 20, 0);
	memset(l4 + n, 0x77, 30);
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n + 30, 5, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(s, b, sizeof(b), 0, NULL, NULL) == 20,
	      "not trimmed to the UDP length");
	// source port 0 is a valid sender
	n = udpd(l4, HX, ME, 0, 9400, d, 10, 0);
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, 6, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(s, b, sizeof(b), 0, NULL, NULL) == 10,
	      "source port 0 refused");
	// what we send carries a valid checksum, never 0
	{
		int t = udp_sock(0, 9401);
		uint8_t r[200], p[2] = { 0, 0 };
		uint16_t c;

		// the payload word that makes the sum 0xffff
		uint8_t u[10];
		n = udpd(u, ME, HX, 9401, 9, p, 2, 1);
		c = inet_csum(u, n, pseudo(ME, HX, 17, n));
		p[0] = (uint8_t)(c >> 8);
		p[1] = (uint8_t)c;
		NS[0]->sendto(t, p, 2, 0, HX, 9);
		n = cap_udp(r, sizeof(r));
		CHECK(n && get16(r + 40) == 0xffff,
		      "checksum %04x where 0 is computed", n ? get16(r + 40) : 0);
		CHECK(!frame_csum_bad(r, n), "checksum wrong");
	}
	return 0;
}

static int t_udp_cork(void)
{
	int s = udp_sock(0, 9500), on = 1, off = 0;
	uint8_t r[1600], b[300];
	unsigned n;

	setup_x();
	pat_fill(5, 0, b, 300);
	CHECK(NS[0]->setsockopt(s, K_SOL_UDP, K_UDP_CORK, &on, sizeof(on)) == 0,
	      "UDP_CORK");
	CHECK(NS[0]->sendto(s, b, 100, 0, HX, 9) == 100, "1");
	CHECK(NS[0]->sendto(s, b + 100, 100, 0, HX, 9) == 100, "2");
	CHECK(NS[0]->sendto(s, b + 200, 100, 0, HY, 99) == 100, "3");
	CHECK(!cap_udp(r, sizeof(r)), "sent while corked");
	CHECK(NS[0]->setsockopt(s, K_SOL_UDP, K_UDP_CORK, &off, sizeof(off)) ==
		      0,
	      "uncork");
	n = cap_udp(r, sizeof(r));
	CHECK(n == 14 + 20 + 8 + 300, "one datagram of %u bytes", n);
	CHECK(get32(r + 30) == HX && get16(r + 36) == 9,
	      "went to %08x:%u, not the first destination", get32(r + 30),
	      get16(r + 36));
	CHECK(!pat_check(5, 0, r + 42, 300) && !frame_csum_bad(r, n),
	      "contents");
	// MSG_MORE
	CHECK(n0_kt_sendmsg(s, b, 50, K_MSG_MORE, HX, 9) == 50, "more 1");
	CHECK(n0_kt_sendmsg(s, b + 50, 50, K_MSG_MORE, HX, 9) == 50, "more 2");
	CHECK(!cap_udp(r, sizeof(r)), "sent with MSG_MORE");
	CHECK(n0_kt_sendmsg(s, b + 100, 50, 0, HX, 9) == 50, "last");
	n = cap_udp(r, sizeof(r));
	CHECK(n == 14 + 20 + 8 + 150 && !pat_check(5, 0, r + 42, 150),
	      "MSG_MORE datagram %u bytes", n);
	return 0;
}

/* Pending (corked) datagrams: buffers sized to what was appended, all of
 * them together bounded, each within twice its send buffer; a failed
 * append discards the datagram. */
static int t_udp_cork_memory(void)
{
	static int socks[200];
	static uint8_t b[2000];
	uint8_t r[1600];
	unsigned long long base, used, out0;
	int ok = 0, nobufs = 0, s, one = 1;
	unsigned n;

	setup_x();
	memset(b, 0x42, sizeof(b));
	base = n0_kt_skb_inuse();
	for (int i = 0; i < 64; i++) {
		socks[i] = udp_sock(0, 0);
		CHECK(n0_kt_sendmsg(socks[i], b, 1, K_MSG_MORE, HX, 9) == 1,
		      "cork %d", i);
	}
	used = n0_kt_skb_inuse() - base;
	CHECK(used <= 64ull * 4096, "64 one-byte corks pin %llu bytes", used);
	for (int i = 64; i < 200; i++) {
		int rc;

		socks[i] = udp_sock(0, 0);
		rc = n0_kt_sendmsg(socks[i], b, 2000, K_MSG_MORE, HX, 9);
		if (rc == 2000)
			ok++;
		else if (rc == -ENOBUFS)
			nobufs++;
	}
	used = n0_kt_skb_inuse() - base;
	CHECK(nobufs > 0 && ok >= 100, "%d pending, %d refused", ok, nobufs);
	CHECK(used <= (8ull << 20) + 64ull * 4096 + 70000,
	      "pending datagrams pin %llu bytes", used);
	for (int i = 0; i < 200; i++)
		NS[0]->close(socks[i]);
	CHECK(n0_kt_skb_inuse() <= base + 16384,
	      "%llu bytes still pinned after close",
	      n0_kt_skb_inuse() - base);
	// twice a small send buffer: the datagram may not outgrow it
	s = udp_sock(0, 0);
	CHECK(NS[0]->setsockopt(s, K_SOL_SOCKET, K_SO_SNDBUF, &one,
				sizeof(one)) == 0,
	      "SO_SNDBUF");
	CHECK(n0_kt_sendmsg(s, b, 1000, K_MSG_MORE, HX, 9) == 1000, "part 1");
	CHECK(n0_kt_sendmsg(s, b, 1000, K_MSG_MORE, HX, 9) == -ENOBUFS,
	      "beyond twice the send buffer");
	cap_start(0, 1);
	CHECK(n0_kt_sendmsg(s, b, 10, 0, HX, 9) == 10, "after the failure");
	n = cap_udp(r, sizeof(r));
	CHECK(n == 14 + 20 + 8 + 10, "the failed datagram was kept: %u", n);
	// the default send buffer: grows with what is appended
	s = udp_sock(0, 0);
	pat_fill(9, 0, b, sizeof(b));
	CHECK(n0_kt_sendmsg(s, b, 1000, K_MSG_MORE, HX, 9) == 1000 &&
		      n0_kt_sendmsg(s, b + 1000, 1000, K_MSG_MORE, HX, 9) ==
			      1000,
	      "grow");
	CHECK(n0_kt_sendmsg(s, b, 0, 0, HX, 9) == 0, "push");
	n = cap_udp(r, sizeof(r));
	CHECK(n > 42 && get16(r + 38) == 8 + 2000,
	      "grown datagram: frame %u, UDP length %u", n,
	      n ? get16(r + 38) : 0);
	CHECK(!pat_check(9, 0, r + 42, n - 42), "grown contents");
	out0 = MIB("UdpOutDatagrams");
	for (int i = 0; i < 20; i++)
		CHECK(n0_kt_sendmsg(s, b, 2000, K_MSG_MORE, HX, 9) == 2000,
		      "append %d", i);
	CHECK(MIB("UdpOutDatagrams") == out0, "sent while pending");
	cap_start(0, 1); // the earlier datagram's second fragment is not of interest
	CHECK(n0_kt_sendmsg(s, b, 7, 0, HX, 9) == 7, "last");
	CHECK(MIB("UdpOutDatagrams") == out0 + 1, "not sent as one");
	n = cap_udp(r, sizeof(r));
	CHECK(n && get16(r + 38) == 8 + 40007, "UDP length %u",
	      n ? get16(r + 38) : 0);
	return 0;
}

/* A frame with a group (multicast) source address: dropped and counted,
 * no warning. */
static int t_eth_group_source(void)
{
	uint8_t f[200], l4[100], d[10] = { 1 }, msrc[6] = { 0x03, 0, 0, 0, 0,
							   0x09 };
	int s = udp_sock(0, 9810);
	unsigned n;

	setup_x();
	n = udpd(l4, HX, ME, 1234, 9810, d, sizeof(d), 0);
	n = ipf(f, msrc, NULL, HX, ME, 17, l4, n, 1, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(s, l4, sizeof(l4), 0, NULL, NULL) == -EAGAIN,
	      "delivered");
	CHECK(n0_kt_dev_stat(0) == 1, "not counted: %llu", n0_kt_dev_stat(0));
	// the same from a unicast source is fine
	n = udpd(l4, HX, ME, 1234, 9810, d, sizeof(d), 0);
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, 2, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(s, l4, sizeof(l4), 0, NULL, NULL) == 10,
	      "a good frame was dropped");
	return 0;
}

/* IP output into a clone: the header goes into a private copy. */
static int t_out_clone_unshared(void)
{
	uint8_t r[1600];
	unsigned n;
	int rc;

	setup_x();
	rc = n0_kt_clone_send(HX);
	CHECK(rc == 0, "step %d", rc);
	n = cap_next_proto(253, r, sizeof(r), 100 * MS);
	CHECK(n == 14 + 20 + 100 && r[34] == 0x5A && r[133] == 0x5A &&
		      !frame_csum_bad(r, n),
	      "sent %u bytes", n);
	return 0;
}

/* An interface without an address takes the DHCP client's unicast
 * replies only. */
static int t_ip_unconfigured(void)
{
	uint8_t f[200], l4[100], d[20] = { 0 }, r[1600];
	int s = udp_sock(0, 9811);
	unsigned long long e0, in0;
	unsigned n;

	setup_x();
	n0_kt_dev_addr(0);
	e0 = MIB("IpInAddrErrors");
	n = udpd(l4, HX, 0x0A000063u, 1234, 9811, d, sizeof(d), 0);
	n = ipf(f, hx_mac, NULL, HX, 0x0A000063u, 17, l4, n, 1, 0, 64, 0,
		NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(s, l4, sizeof(l4), 0, NULL, NULL) == -EAGAIN,
	      "an address-less interface took a datagram for any address");
	CHECK(MIB("IpInAddrErrors") == e0 + 1, "not counted");
	// nor an echo request
	n = echo_from(f, HX, 0x0A000063u, NULL, 1, NULL, 0);
	CHECK(!rx_and_reply(f, n, 1, r, sizeof(r)), "answered an echo");
	// the DHCP client's reply is taken
	in0 = MIB("UdpInDatagrams");
	n = udpd(l4, HX, 0x0A000063u, 67, 68, d, sizeof(d), 0);
	n = ipf(f, hx_mac, NULL, HX, 0x0A000063u, 17, l4, n, 3, 0, 64, 0,
		NULL, 0);
	NS[0]->rx(f, n);
	CHECK(MIB("UdpInDatagrams") == in0 + 1, "the DHCP reply was dropped");
	n0_kt_dev_addr(ME);
	return 0;
}

/* The device's multicast list follows the memberships. */
static int t_igmp_mc_list(void)
{
	int a = udp_sock(0, 0), b = udp_sock(0, 0);

	setup_x();
	CHECK(n0_kt_mc_join(a, 0xEF060606u, 0, 0) == 0 &&
		      n0_kt_mc_join(b, 0xEF060606u, 0, 0) == 0,
	      "join");
	CHECK(n0_kt_dev_stat(2) == 1 && n0_kt_dev_stat(3) == 1,
	      "list: %llu entries, %llu users", n0_kt_dev_stat(2),
	      n0_kt_dev_stat(3));
	CHECK(n0_kt_mc_join(a, 0xEF060606u, 0, 1) == 0, "leave");
	CHECK(n0_kt_dev_stat(2) == 1, "left the list with a member left");
	CHECK(n0_kt_mc_join(b, 0xEF060606u, 0, 1) == 0, "leave");
	CHECK(n0_kt_dev_stat(2) == 0, "still on the list after the last "
	      "leave");
	CHECK(n0_kt_mc_join(a, 0xEF060606u, 0, 0) == 0 &&
		      n0_kt_dev_stat(2) == 1,
	      "join again");
	NS[0]->close(a);
	CHECK(n0_kt_dev_stat(2) == 0, "close did not leave");
	NS[0]->close(b);
	return 0;
}

static int t_udp_errors(void)
{
	int c = udp_sock(0, 9600), u = udp_sock(0, 9601), on = 1;
	uint8_t f[200], l4[200], q[28], b[100];
	struct kt_msg m;
	unsigned n;

	setup_x();
	CHECK(NS[0]->connect(c, HX, 53) == 0, "connect");
	// port unreachable for the connected socket: ECONNREFUSED, once
	memset(q, 0, sizeof(q));
	q[0] = 0x45;
	put16(q + 2, 40);
	q[8] = 64;
	q[9] = 17;
	put32(q + 12, ME);
	put32(q + 16, HX);
	put16(q + 10, inet_csum(q, 20, 0));
	put16(q + 20, 9600);
	put16(q + 22, 53);
	put16(q + 24, 20);
	n = icmpm(l4, 3, 3, 0, q, 28);
	n = ipf(f, hx_mac, NULL, HX, ME, 1, l4, n, 1, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(c, b, sizeof(b), 0, NULL, NULL) == -ECONNREFUSED,
	      "connected socket: no ECONNREFUSED");
	CHECK(NS[0]->recvfrom(c, b, sizeof(b), 0, NULL, NULL) == -EAGAIN,
	      "reported twice");
	// a soft error (host unreachable) is not reported without RECVERR
	q[0] = 0x45;
	n = icmpm(l4, 3, 1, 0, q, 28);
	n = ipf(f, hx_mac, NULL, HX, ME, 1, l4, n, 2, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(c, b, sizeof(b), 0, NULL, NULL) == -EAGAIN,
	      "soft error reported");
	// unconnected: nothing without IP_RECVERR...
	put16(q + 20, 9601);
	put16(q + 10, 0);
	put16(q + 10, inet_csum(q, 20, 0));
	n = icmpm(l4, 3, 3, 0, q, 28);
	n = ipf(f, hx_mac, NULL, HX, ME, 1, l4, n, 3, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->recvfrom(u, b, sizeof(b), 0, NULL, NULL) == -EAGAIN,
	      "unconnected socket got an error");
	// ...with it, the error queue, the offender and the destination
	NS[0]->setsockopt(u, K_IPPROTO_IP, K_IP_RECVERR, &on, sizeof(on));
	{
		uint8_t q2[36];

		memcpy(q2, q, 28);
		memcpy(q2 + 28, "PAYLOAD!", 8);
		put16(q2 + 2, 36);
		put16(q2 + 10, 0);
		put16(q2 + 10, inet_csum(q2, 20, 0));
		n = icmpm(l4, 3, 3, 0, q2, 36);
		// from a router (HY), about our datagram to HX
		n = ipf(f, hy_mac, NULL, HY, ME, 1, l4, n, 4, 0, 64, 0, NULL,
			0);
		NS[0]->rx(f, n);
	}
	CHECK(n0_kt_recvmsg(u, b, sizeof(b), K_MSG_ERRQUEUE, &m) == 8,
	      "error queue read %d", m.ret);
	CHECK(!memcmp(b, "PAYLOAD!", 8), "error payload");
	CHECK(m.have_err && m.ee_errno == ECONNREFUSED && m.ee_origin == 2 &&
		      m.ee_type == 3 && m.ee_code == 3 && m.offender == HY,
	      "ee errno %u origin %u %u/%u offender %08x", m.ee_errno,
	      m.ee_origin, m.ee_type, m.ee_code, m.offender);
	CHECK(m.src_ip == HX && m.src_port == 53, "msg_name %08x:%u",
	      m.src_ip, m.src_port);
	CHECK(m.flags & K_MSG_ERRQUEUE, "MSG_ERRQUEUE missing");
	// the pending error went with it
	CHECK(NS[0]->recvfrom(u, b, sizeof(b), 0, NULL, NULL) == -EAGAIN,
	      "pending error after the queue was read");
	// POLLERR while queued
	n = icmpm(l4, 3, 3, 0, q, 28);
	n = ipf(f, hy_mac, NULL, HY, ME, 1, l4, n, 5, 0, 64, 0, NULL, 0);
	NS[0]->rx(f, n);
	CHECK(NS[0]->poll(u, K_POLLIN) & K_POLLERR, "no POLLERR");
	// a quote that is too short is ignored
	{
		unsigned long long e0 = MIB("IcmpInErrors");

		n = icmpm(l4, 3, 3, 0, q, 24);
		n = ipf(f, hy_mac, NULL, HY, ME, 1, l4, n, 6, 0, 64, 0, NULL,
			0);
		NS[0]->rx(f, n);
		CHECK(MIB("IcmpInErrors") == e0 + 1, "short quote not counted");
	}
	return 0;
}

/* Over the wire: loopback-free interop of the two stacks. */
static int t_udp_interop_64k(void)
{
	int a = udp_sock(0, 0), b = udp_sock(1, 11000);
	uint8_t *d = malloc(65507), *r = malloc(65536);
	unsigned ip, port;

	pat_fill(77, 0, d, 65507);
	CHECK(NS[0]->sendto(a, d, 65507, 0, inst_ip(1), 11000) == 65507, "send");
	sim_run_for(200 * MS);
	int n = NS[1]->recvfrom(b, r, 65536, 0, &ip, &port);
	CHECK(n == 65507 && !pat_check(77, 0, r, 65507), "received %d", n);
	CHECK(ip == inst_ip(0), "from %08x", ip);
	CHECK(n1_kt_mib("IpReasmOKs") == 1, "reassembled %llu",
	      n1_kt_mib("IpReasmOKs"));
	free(d);
	free(r);
	return 0;
}

static int t_raw_icmp(void)
{
	int rs = n0_kt_raw_socket(1);
	uint8_t f[1600], r[1600];
	unsigned n;

	setup_x();
	CHECK(rs >= 0, "raw socket");
	NS[0]->set_nonblock(rs, 1);
	n = echo_from(f, HX, ME, NULL, 1, NULL, 0);
	NS[0]->rx(f, n);
	// the kernel still answers, and the raw socket sees the request
	CHECK(cap_next_proto(1, r, sizeof(r), 100 * MS), "no echo reply");
	n = (unsigned)NS[0]->recvfrom(rs, r, sizeof(r), 0, NULL, NULL);
	CHECK(n == 20 + 8 + 64 && r[0] == 0x45 && r[20] == 8,
	      "raw socket got %d bytes type %u", (int)n, r[20]);
	// a raw socket of another protocol sees nothing
	{
		int r17 = n0_kt_raw_socket(17);

		NS[0]->set_nonblock(r17, 1);
		NS[0]->rx(f, echo_from(f, HX, ME, NULL, 2, NULL, 0));
		CHECK(NS[0]->recvfrom(r17, r, sizeof(r), 0, NULL, NULL) ==
			      -EAGAIN,
		      "UDP raw socket got ICMP");
	}
	// sending: an ICMP payload goes out with our header
	{
		uint8_t l4[40];
		unsigned m = icmpm(l4, 8, 0, 0x99, NULL, 0);

		cap_start(0, 1); // what was sent so far is not of interest

		CHECK(NS[0]->sendto(rs, l4, m, 0, HX, 0) == (int)m, "raw send");
		n = cap_next_proto(1, r, sizeof(r), 100 * MS);
		CHECK(n == 14 + 20 + 8 && r[34] == 8, "raw send: %u bytes", n);
	}
	return 0;
}

/* IGMPv2: a report on join and once more within 10 s; a query is
 * answered after a random delay within its Max Response Time. */
static int t_igmp_reports(void)
{
	uint8_t r[1600], f[200], q[8];
	simtime_t t0;
	unsigned n;
	int s = udp_sock(0, 0);

	setup_x();
	CHECK(n0_kt_mc_join(s, 0xEF050505u, 0, 0) == 0, "join");
	n = cap_next_proto(2, r, sizeof(r), 10 * MS);
	CHECK(n && r[34] == 0x16 && get32(r + 38) == 0xEF050505u,
	      "no report on join");
	CHECK(r[22] == 1 && get32(r + 30) == 0xEF050505u, "ttl %u to %08x",
	      r[22], get32(r + 30));
	t0 = now_ns();
	n = cap_next_proto(2, r, sizeof(r), 11 * SEC);
	CHECK(n && r[34] == 0x16, "the report was not repeated");
	CHECK(now_ns() - t0 <= 10 * SEC + 50 * MS, "repeated after %llu ms",
	      (now_ns() - t0) / MS);
	// a general query, 1 s to answer
	q[0] = 0x11;
	q[1] = 10;
	q[2] = q[3] = 0;
	put32(q + 4, 0);
	put16(q + 2, inet_csum(q, 8, 0));
	n = ipf(f, hx_mac, NULL, HX, 0xE0000001u, 2, q, 8, 1, 0, 1, 0, NULL, 0);
	f[0] = 0x01;
	f[1] = 0x00;
	f[2] = 0x5e;
	f[3] = 0;
	f[4] = 0;
	f[5] = 1;
	t0 = now_ns();
	NS[0]->rx(f, n);
	n = cap_next_proto(2, r, sizeof(r), 2 * SEC);
	CHECK(n && r[34] == 0x16 && get32(r + 38) == 0xEF050505u,
	      "query not answered");
	CHECK(now_ns() - t0 <= 1 * SEC + 20 * MS, "answered after %llu ms",
	      (now_ns() - t0) / MS);
	CHECK(!cap_next_proto(2, r, sizeof(r), 2 * SEC),
	      "answered more than once");
	// leaving sends a leave to all-routers
	CHECK(n0_kt_mc_join(s, 0xEF050505u, 0, 1) == 0, "leave");
	n = cap_next_proto(2, r, sizeof(r), 10 * MS);
	CHECK(n && r[34] == 0x17 && get32(r + 30) == 0xE0000002u, "no leave");
	return 0;
}

/* Receive checksum offload: a frame the NIC verified
 * (CHECKSUM_UNNECESSARY) is not checked again; one it did not is. */
static int t_rx_csum_offload(void)
{
	uint8_t f[1600], l4[200], d[40], r[1600];
	int s = udp_sock(0, 9700);
	unsigned n;

	setup_x();
	pat_fill(1, 0, d, sizeof(d));
	n = udpd(l4, HX, ME, 1234, 9700, d, 40, 2); // wrong checksum
	n = ipf(f, hx_mac, NULL, HX, ME, 17, l4, n, 1, 0, 64, 0, NULL, 0);
	n0_kt_rx_skb(f, n, 0);
	CHECK(NS[0]->recvfrom(s, r, sizeof(r), 0, NULL, NULL) == -EAGAIN,
	      "UDP: bad checksum accepted without offload");
	n0_kt_rx_skb(f, n, 1);
	CHECK(NS[0]->recvfrom(s, r, sizeof(r), 0, NULL, NULL) == 40,
	      "UDP: a verified datagram was checked again");
	// TCP: a SYN to a listener with a (deliberately) wrong checksum
	{
		struct tcpseg sg;
		int ls = NS[0]->socket(K_SOCK_STREAM);

		NS[0]->set_nonblock(ls, 1);
		CHECK(NS[0]->bind(ls, 0, 9800) == 0 && NS[0]->listen(ls, 4) == 0,
		      "listen");
		tcpseg_init(&sg);
		sg.sip = HX;
		sg.dip = ME;
		sg.sport = 33333;
		sg.dport = 9800;
		sg.seq = 1000;
		sg.flags = F_SYN;
		sg.win = 65535;
		sg.mss = 1460;
		my_mac(0, r);
		n = pkt_tcp(f, r, hx_mac, &sg);
		f[14 + 20 + 16] ^= 0x55;
		n0_kt_rx_skb(f, n, 0);
		CHECK(!cap_next_proto(6, r, sizeof(r), 50 * MS),
		      "TCP: answered a bad checksum without offload");
		n0_kt_rx_skb(f, n, 1);
		CHECK(cap_next_proto(6, r, sizeof(r), 50 * MS),
		      "TCP: a verified SYN was checked again");
	}
	return 0;
}

#define T(n, f, ...) { .name = n, .group = "unit", .fn = f, __VA_ARGS__ }
#define TI(n, f, ...) { .name = n, .group = "interop", .fn = f, __VA_ARGS__ }
const struct test tests_ip[] = {
	T("ip-header-checks", t_ip_header_checks, .ninst = 1),
	T("ip-martians", t_ip_martians, .ninst = 1),
	T("ip-unknown-proto", t_ip_unknown_proto, .ninst = 1),
	T("ip-options", t_ip_options, .ninst = 1),
	T("ip-source-route", t_ip_source_route, .ninst = 1),
	T("frag-dup-and-order", t_frag_dup_and_order, .ninst = 1),
	T("frag-overlap", t_frag_overlap, .ninst = 1),
	T("frag-timeout", t_frag_timeout, .ninst = 1),
	T("frag-memory", t_frag_memory, .ninst = 1),
	T("frag-limits", t_frag_limits, .ninst = 1),
	T("out-df-and-id", t_out_df_and_id, .ninst = 1),
	T("out-fragments", t_out_fragments, .ninst = 1),
	T("pmtu-do", t_pmtu_do, .ninst = 1),
	T("pmtu-frag-needed", t_pmtu_frag_needed, .ninst = 1),
	T("pmtu-report-rules", t_pmtu_report_rules, .ninst = 1),
	T("icmp-broadcast-echo", t_icmp_broadcast_echo, .ninst = 1),
	T("icmp-timestamp", t_icmp_timestamp, .ninst = 1),
	T("icmp-error-rules", t_icmp_error_rules, .ninst = 1),
	T("icmp-rate-limits", t_icmp_rate_limits, .ninst = 1),
	T("udp-lookup", t_udp_lookup, .ninst = 1),
	T("udp-bind-rules", t_udp_bind_rules, .ninst = 1),
	T("udp-reuseport", t_udp_reuseport, .ninst = 1),
	T("udp-broadcast-fanout", t_udp_broadcast_fanout, .ninst = 1),
	T("udp-multicast-out", t_udp_multicast_out, .ninst = 1),
	T("udp-rcvbuf", t_udp_rcvbuf, .ninst = 1),
	T("udp-recv-flags", t_udp_recv_flags, .ninst = 1),
	T("udp-cmsgs", t_udp_cmsgs, .ninst = 1),
	T("udp-checksums", t_udp_checksums, .ninst = 1),
	T("udp-cork", t_udp_cork, .ninst = 1),
	T("udp-cork-memory", t_udp_cork_memory, .ninst = 1),
	T("eth-group-source", t_eth_group_source, .ninst = 1),
	T("out-clone-unshared", t_out_clone_unshared, .ninst = 1),
	T("ip-unconfigured", t_ip_unconfigured, .ninst = 1),
	T("igmp-mc-list", t_igmp_mc_list, .ninst = 1),
	T("udp-errors", t_udp_errors, .ninst = 1),
	T("raw-icmp", t_raw_icmp, .ninst = 1),
	T("igmp-reports", t_igmp_reports, .ninst = 1),
	T("rx-csum-offload", t_rx_csum_offload, .ninst = 1),
	TI("udp-64k", t_udp_interop_64k),
	{ 0 },
};
